#include "transcription_service.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "followup_task_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

namespace transcription_service {
namespace {

constexpr const char* kTag = "TranscriptionSvc";
constexpr uint32_t kWorkerTaskStackWords = 8192;

constexpr const char* kOpenAiNvsNamespace = "openai";
constexpr const char* kOpenAiApiKeyKey = "api_key";
constexpr const char* kOpenAiTranscriptionUrl = "https://api.openai.com/v1/audio/transcriptions";
constexpr const char* kOpenAiTranscriptionModel = "gpt-4o-mini-transcribe";
constexpr const char* kOpenAiSettingsUri = "/api/settings/openai";
constexpr const char* kMultipartBoundary = "----PocketClankerBoundary7MA4YWxkTrZu0gW";
constexpr int kOpenAiTimeoutMs = 45000;
constexpr size_t kPortalPayloadMax = 768;

struct TaskContext {
    recording_service::RecordedClipPtr clip = {};
};

struct OpenAiResult {
    bool success = false;
    int http_status = 0;
    std::string transcript = {};
    std::string error_code = {};
    std::string error_message = {};
};

#pragma pack(push, 1)
struct WavHeader {
    char riff[4];
    uint32_t chunk_size;
    char wave[4];
    char fmt[4];
    uint32_t subchunk1_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
};
#pragma pack(pop)

static_assert(sizeof(WavHeader) == 44, "WAV header must be 44 bytes");

std::mutex s_mutex;
EventHandler s_event_handler = nullptr;
void* s_event_context = nullptr;
bool s_initialized = false;
bool s_request_in_flight = false;
int s_last_http_status = 0;
std::string s_last_status_message = {};
std::string s_last_error_code = {};
std::string s_last_error_message = {};
std::string s_last_transcript = {};
std::string s_openai_api_key = {};

std::string TrimCopy(std::string value)
{
    const auto not_space = [](unsigned char ch) { return ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n'; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

bool LoadOpenAiApiKeyFromNvs(std::string* out)
{
    if (out == nullptr) {
        return false;
    }
    nvs_handle_t handle = 0;
    const esp_err_t open_err = nvs_open(kOpenAiNvsNamespace, NVS_READONLY, &handle);
    if (open_err != ESP_OK) {
        out->clear();
        return false;
    }

    size_t size = 0;
    esp_err_t err = nvs_get_str(handle, kOpenAiApiKeyKey, nullptr, &size);
    if (err != ESP_OK || size <= 1) {
        nvs_close(handle);
        out->clear();
        return false;
    }

    std::string value(size, '\0');
    err = nvs_get_str(handle, kOpenAiApiKeyKey, value.data(), &size);
    nvs_close(handle);
    if (err != ESP_OK) {
        out->clear();
        return false;
    }
    if (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    *out = TrimCopy(std::move(value));
    return !out->empty();
}

bool SaveOpenAiApiKeyToNvs(const std::string& api_key)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kOpenAiNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return false;
    }
    err = nvs_set_str(handle, kOpenAiApiKeyKey, api_key.c_str());
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

Snapshot BuildSnapshotLocked()
{
    Snapshot snapshot = {};
    snapshot.initialized = s_initialized;
    snapshot.provider_ready = !s_openai_api_key.empty();
    snapshot.request_in_flight = s_request_in_flight;
    snapshot.last_http_status = s_last_http_status;
    snapshot.last_status_message = s_last_status_message;
    snapshot.last_error_code = s_last_error_code;
    snapshot.last_error_message = s_last_error_message;
    snapshot.last_transcript = s_last_transcript;
    return snapshot;
}

void NotifyLocked()
{
    EventHandler handler = s_event_handler;
    void* context = s_event_context;
    if (handler == nullptr) {
        return;
    }
    const Event event = {.snapshot = BuildSnapshotLocked()};
    handler(event, context);
}

WavHeader BuildWavHeader(const recording_service::RecordedClip& clip)
{
    const uint32_t data_size = static_cast<uint32_t>(clip.pcm16_byte_count());
    WavHeader header = {};
    std::memcpy(header.riff, "RIFF", 4);
    header.chunk_size = 36U + data_size;
    std::memcpy(header.wave, "WAVE", 4);
    std::memcpy(header.fmt, "fmt ", 4);
    header.subchunk1_size = 16;
    header.audio_format = 1;
    header.num_channels = 1;
    header.sample_rate = clip.sample_rate_hz();
    header.byte_rate = clip.sample_rate_hz() * sizeof(int16_t);
    header.block_align = sizeof(int16_t);
    header.bits_per_sample = 16;
    std::memcpy(header.data, "data", 4);
    header.data_size = data_size;
    return header;
}

bool HttpWriteAll(esp_http_client_handle_t client, const char* data, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        const int written = esp_http_client_write(client, data + offset, len - offset);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

std::string JsonErrorMessage(cJSON* root)
{
    if (root == nullptr) {
        return {};
    }
    cJSON* error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (!cJSON_IsObject(error)) {
        return {};
    }
    cJSON* message = cJSON_GetObjectItemCaseSensitive(error, "message");
    return cJSON_IsString(message) && message->valuestring != nullptr ? message->valuestring : "";
}

std::string JsonErrorCode(cJSON* root)
{
    if (root == nullptr) {
        return {};
    }
    cJSON* error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (!cJSON_IsObject(error)) {
        return {};
    }
    cJSON* code = cJSON_GetObjectItemCaseSensitive(error, "code");
    if (cJSON_IsString(code) && code->valuestring != nullptr) {
        return code->valuestring;
    }
    cJSON* type = cJSON_GetObjectItemCaseSensitive(error, "type");
    return cJSON_IsString(type) && type->valuestring != nullptr ? type->valuestring : "";
}

OpenAiResult TranscribeWithOpenAi(const recording_service::RecordedClip& clip,
                                  const std::string& api_key)
{
    OpenAiResult result = {};
    if (api_key.empty()) {
        result.error_code = "not_configured";
        result.error_message = "No OpenAI API key configured";
        return result;
    }

    std::string prefix;
    prefix.reserve(384);
    prefix += "--";
    prefix += kMultipartBoundary;
    prefix += "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n";
    prefix += kOpenAiTranscriptionModel;
    prefix += "\r\n--";
    prefix += kMultipartBoundary;
    prefix += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"recording.wav\"\r\n";
    prefix += "Content-Type: audio/wav\r\n\r\n";

    std::string suffix = "\r\n--";
    suffix += kMultipartBoundary;
    suffix += "--\r\n";

    const WavHeader wav_header = BuildWavHeader(clip);
    const size_t content_length =
        prefix.size() + sizeof(wav_header) + clip.pcm16_byte_count() + suffix.size();

    esp_http_client_config_t config = {};
    config.url = kOpenAiTranscriptionUrl;
    config.method = HTTP_METHOD_POST;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = kOpenAiTimeoutMs;
    config.buffer_size = 2048;
    config.buffer_size_tx = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        result.error_code = "http_client_init_failed";
        result.error_message = "Failed to initialize OpenAI HTTP client";
        return result;
    }

    const std::string auth_header = "Bearer " + api_key;
    const std::string content_type = std::string("multipart/form-data; boundary=") + kMultipartBoundary;
    esp_http_client_set_header(client, "Authorization", auth_header.c_str());
    esp_http_client_set_header(client, "Content-Type", content_type.c_str());
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "pocket-clanker");

    esp_err_t err = esp_http_client_open(client, static_cast<int>(content_length));
    if (err != ESP_OK) {
        result.error_code = "transport_error";
        result.error_message = esp_err_to_name(err);
        esp_http_client_cleanup(client);
        return result;
    }

    bool write_ok = HttpWriteAll(client, prefix.data(), prefix.size()) &&
                    HttpWriteAll(client, reinterpret_cast<const char*>(&wav_header), sizeof(wav_header));
    if (write_ok) {
        clip.ForEachChunk([&](const int16_t* samples, size_t sample_count) {
            if (!write_ok || samples == nullptr || sample_count == 0) {
                return;
            }
            write_ok = HttpWriteAll(client, reinterpret_cast<const char*>(samples),
                                    sample_count * sizeof(int16_t));
        });
    }
    if (write_ok) {
        write_ok = HttpWriteAll(client, suffix.data(), suffix.size());
    }

    if (!write_ok) {
        result.error_code = "upload_failed";
        result.error_message = "Failed while uploading audio to OpenAI";
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return result;
    }

    const int64_t headers = esp_http_client_fetch_headers(client);
    if (headers < 0) {
        result.error_code = "transport_error";
        result.error_message = "Failed to read OpenAI response headers";
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return result;
    }

    result.http_status = esp_http_client_get_status_code(client);
    std::string body;
    std::array<char, 1024> buffer = {};
    while (true) {
        const int read = esp_http_client_read(client, buffer.data(), buffer.size());
        if (read < 0) {
            result.error_code = "response_read_failed";
            result.error_message = "Failed to read OpenAI response";
            break;
        }
        if (read == 0) {
            break;
        }
        body.append(buffer.data(), static_cast<size_t>(read));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!result.error_code.empty()) {
        return result;
    }

    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (result.http_status >= 200 && result.http_status < 300 && root != nullptr) {
        cJSON* text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring != nullptr) {
            result.transcript = TrimCopy(text->valuestring);
            result.success = !result.transcript.empty();
        }
        if (!result.success) {
            result.error_code = "empty_transcript";
            result.error_message = "OpenAI returned no transcript text";
        }
    } else {
        result.error_code = JsonErrorCode(root);
        if (result.error_code.empty()) {
            result.error_code = "openai_http_error";
        }
        result.error_message = JsonErrorMessage(root);
        if (result.error_message.empty()) {
            result.error_message = body.empty() ? "OpenAI transcription request failed" : body.substr(0, 160);
        }
    }

    if (root != nullptr) {
        cJSON_Delete(root);
    }
    return result;
}

void WorkerTask(void* raw_context)
{
    std::unique_ptr<TaskContext> context(static_cast<TaskContext*>(raw_context));
    if (!context || !context->clip || context->clip->empty()) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_request_in_flight = false;
        s_last_http_status = 0;
        s_last_status_message = "Transcription failed";
        s_last_error_code = "empty_audio";
        s_last_error_message = "No recorded audio available";
        s_last_transcript.clear();
        NotifyLocked();
        vTaskDelete(nullptr);
        return;
    }

    std::string api_key;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        api_key = s_openai_api_key;
    }

    const OpenAiResult result = TranscribeWithOpenAi(*context->clip, api_key);

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_request_in_flight = false;
        s_last_http_status = result.http_status;
        if (result.success) {
            s_last_status_message = "Transcript ready";
            s_last_error_code.clear();
            s_last_error_message.clear();
            s_last_transcript = result.transcript;
            ESP_LOGI(kTag, "OpenAI transcription succeeded: chars=%u wav_bytes=%u clip_ms=%u",
                     static_cast<unsigned>(s_last_transcript.size()),
                     static_cast<unsigned>(context->clip->wav_byte_count()),
                     static_cast<unsigned>(context->clip->duration_ms()));
        } else {
            s_last_status_message = "Transcription failed";
            s_last_error_code = result.error_code;
            s_last_error_message = result.error_message;
            s_last_transcript.clear();
            ESP_LOGW(kTag, "OpenAI transcription failed: http=%d code=%s message=%s",
                     result.http_status,
                     s_last_error_code.empty() ? "<none>" : s_last_error_code.c_str(),
                     s_last_error_message.empty() ? "<none>" : s_last_error_message.c_str());
        }
        NotifyLocked();
    }

    vTaskDelete(nullptr);
}

std::string ReadRequestBody(httpd_req_t* request)
{
    if (request == nullptr || request->content_len <= 0) {
        return {};
    }
    std::string body(static_cast<size_t>(request->content_len), '\0');
    size_t offset = 0;
    while (offset < body.size()) {
        const int received = httpd_req_recv(request, body.data() + offset, body.size() - offset);
        if (received <= 0) {
            return {};
        }
        offset += static_cast<size_t>(received);
    }
    return body;
}

esp_err_t SendJson(httpd_req_t* request, int status, cJSON* root)
{
    char* raw = cJSON_PrintUnformatted(root);
    std::string payload = raw != nullptr ? raw : "{}";
    if (raw != nullptr) {
        cJSON_free(raw);
    }
    cJSON_Delete(root);

    httpd_resp_set_status(request, status == 200 ? HTTPD_200 : HTTPD_400);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    return httpd_resp_send(request, payload.c_str(), payload.size());
}

esp_err_t HandleOpenAiSettingsGet(httpd_req_t* request)
{
    std::string key;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        key = s_openai_api_key;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddBoolToObject(root, "configured", !key.empty());
    const std::string last4 = key.size() >= 4 ? key.substr(key.size() - 4) : "";
    cJSON_AddStringToObject(root, "last4", last4.c_str());
    cJSON_AddStringToObject(root, "model", kOpenAiTranscriptionModel);
    return SendJson(request, 200, root);
}

esp_err_t HandleOpenAiSettingsPost(httpd_req_t* request)
{
    if (request == nullptr || request->content_len <= 0 ||
        request->content_len > static_cast<int>(kPortalPayloadMax)) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "message", "Invalid OpenAI settings payload");
        return SendJson(request, 400, root);
    }

    const std::string body = ReadRequestBody(request);
    cJSON* parsed = cJSON_ParseWithLength(body.c_str(), body.size());
    cJSON* key_item = parsed != nullptr
                          ? cJSON_GetObjectItemCaseSensitive(parsed, "api_key")
                          : nullptr;
    std::string api_key =
        cJSON_IsString(key_item) && key_item->valuestring != nullptr ? key_item->valuestring : "";
    api_key = TrimCopy(std::move(api_key));
    if (parsed != nullptr) {
        cJSON_Delete(parsed);
    }

    if (api_key.empty()) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "message", "OpenAI API key required");
        return SendJson(request, 400, root);
    }

    if (!SaveOpenAiApiKeyToNvs(api_key)) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "message", "Failed to save OpenAI API key");
        return SendJson(request, 400, root);
    }

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_openai_api_key = api_key;
        s_last_status_message = "OpenAI configured";
        s_last_error_code.clear();
        s_last_error_message.clear();
        NotifyLocked();
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "message", "OpenAI API key saved");
    cJSON_AddStringToObject(root, "last4",
                            api_key.size() >= 4 ? api_key.substr(api_key.size() - 4).c_str() : "");
    return SendJson(request, 200, root);
}

}  // namespace

esp_err_t Init()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_initialized) {
        return ESP_OK;
    }

    (void)LoadOpenAiApiKeyFromNvs(&s_openai_api_key);
    s_initialized = true;
    s_request_in_flight = false;
    s_last_http_status = 0;
    s_last_status_message = s_openai_api_key.empty()
                                ? "OpenAI transcription unavailable"
                                : "OpenAI ready for transcription";
    s_last_error_code.clear();
    s_last_error_message.clear();
    s_last_transcript.clear();
    return ESP_OK;
}

void SetEventHandler(EventHandler handler, void* context)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_event_handler = handler;
    s_event_context = context;
}

Snapshot GetSnapshot()
{
    if (!s_initialized) {
        (void)Init();
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    return BuildSnapshotLocked();
}

bool HasOpenAiApiKey()
{
    if (!s_initialized) {
        (void)Init();
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    return !s_openai_api_key.empty();
}

bool BeginTranscription(recording_service::RecordedClipPtr clip)
{
    if (Init() != ESP_OK) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_request_in_flight) {
            s_last_status_message = "Transcription already running";
            s_last_error_code = "request_in_flight";
            s_last_error_message = "A transcription request is already running";
            NotifyLocked();
            return false;
        }
        if (s_openai_api_key.empty()) {
            s_last_http_status = 0;
            s_last_status_message = "Transcription unavailable";
            s_last_error_code = "not_configured";
            s_last_error_message = "No OpenAI API key configured";
            s_last_transcript.clear();
            NotifyLocked();
            return false;
        }
        if (!clip || clip->empty()) {
            s_last_http_status = 0;
            s_last_status_message = "Transcription unavailable";
            s_last_error_code = "empty_audio";
            s_last_error_message = "No recorded audio available";
            s_last_transcript.clear();
            NotifyLocked();
            return false;
        }

        s_request_in_flight = true;
        s_last_http_status = 0;
        s_last_status_message = "Transcribing recording";
        s_last_error_code.clear();
        s_last_error_message.clear();
        s_last_transcript.clear();
        NotifyLocked();
    }

    TaskContext* task_context = new (std::nothrow) TaskContext();
    if (task_context == nullptr) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_request_in_flight = false;
        s_last_status_message = "Transcription unavailable";
        s_last_error_code = "task_context_alloc_failed";
        s_last_error_message = "Failed to allocate transcription task context";
        NotifyLocked();
        return false;
    }
    task_context->clip = std::move(clip);

    TaskHandle_t task = nullptr;
    const BaseType_t created = xTaskCreatePinnedToCore(
        WorkerTask, "transcription", kWorkerTaskStackWords, task_context,
        followup_task_config::kPriorityGemini, &task, followup_task_config::kSystemCore);
    if (created != pdPASS) {
        delete task_context;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_request_in_flight = false;
        s_last_status_message = "Transcription unavailable";
        s_last_error_code = "task_start_failed";
        s_last_error_message = "Failed to queue transcription task";
        NotifyLocked();
        return false;
    }

    ESP_LOGI(kTag, "Starting OpenAI transcription: samples=%u",
             static_cast<unsigned>(task_context->clip->sample_count()));
    return true;
}

void RegisterPortalRoutes(httpd_handle_t server)
{
    if (server == nullptr) {
        return;
    }
    (void)Init();

    httpd_uri_t get_settings = {
        .uri = kOpenAiSettingsUri,
        .method = HTTP_GET,
        .handler = HandleOpenAiSettingsGet,
        .user_ctx = nullptr,
    };
    httpd_uri_t post_settings = {
        .uri = kOpenAiSettingsUri,
        .method = HTTP_POST,
        .handler = HandleOpenAiSettingsPost,
        .user_ctx = nullptr,
    };

    esp_err_t err = httpd_register_uri_handler(server, &get_settings);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Register OpenAI settings GET failed: %s", esp_err_to_name(err));
    }
    err = httpd_register_uri_handler(server, &post_settings);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Register OpenAI settings POST failed: %s", esp_err_to_name(err));
    }
}

}  // namespace transcription_service
