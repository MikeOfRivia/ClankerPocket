#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <esp_err.h>
#include <lvgl.h>
#include "esp_lv_adapter_arduino.h"
#include "mcu-max.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

static lv_obj_t *cells[64], *pieces[64], *statusLabel, *modeLabel, *levelLabel, *lastLabel;
static lv_obj_t *screen;
static volatile int queuedSquare=-1;
static volatile int queuedAction=0;
static bool vsAI=true, awaitingAI=false, gameOver=false;
static uint8_t level=2;
static int selected=-1;
static mcumax_move validMoves[256];
static uint32_t validCount=0;
static mcumax_move history[512];
static int historyCount=0;
static char lastText[56]="Tap a piece to begin";
static uint8_t renderedPiece[64];
static uint32_t renderedColor[64];
static const uint32_t limits[]={350,1500,6000,18000,45000};
static const uint8_t depths[]={2,3,4,5,6};

static bool sameMove(mcumax_move a,mcumax_move b){return a.from==b.from&&a.to==b.to;}
static bool hasFrom(uint8_t sq){for(uint32_t i=0;i<validCount&&i<256;i++)if(validMoves[i].from==sq)return true;return false;}
static bool canMove(uint8_t from,uint8_t to){for(uint32_t i=0;i<validCount&&i<256;i++)if(validMoves[i].from==from&&validMoves[i].to==to)return true;return false;}

static void refreshLegal(){
 validCount=mcumax_search_valid_moves(validMoves,256);
 gameOver=(validCount==0);
}

static char pieceLetter(mcumax_piece p){
 switch(p&7){
  case MCUMAX_PAWN_UPSTREAM: case MCUMAX_PAWN_DOWNSTREAM:return 'P';
  case MCUMAX_KNIGHT:return 'N';
  case MCUMAX_BISHOP:return 'B';
  case MCUMAX_ROOK:return 'R';
  case MCUMAX_QUEEN:return 'Q';
  case MCUMAX_KING:return 'K';
  default:return '\0';
 }
}
static void squareName(uint8_t s,char out[3]){
 out[0]='a'+(s&7);out[1]='8'-(s>>4);out[2]=0;
}
static void moveText(mcumax_move m,bool ai){
 char a[3],b[3];squareName(m.from,a);squareName(m.to,b);
 snprintf(lastText,sizeof(lastText),"%s: %s to %s",ai?"Computer":"Player",a,b);
}

static void drawBoard(){
 const bool blackToMove=(mcumax_get_current_side()==0x10);
 for(int i=0;i<64;i++){
  uint8_t sq=((i>>3)<<4)|(i&7);
  mcumax_piece p=mcumax_get_piece(sq);
  char letter=pieceLetter(p);
  char t[2]={letter,0};
  // Only touch LVGL objects that actually changed. Rebuilding 64 labels and
  // restyling 64 buttons for a single selection caused excessive redraw work.
  if(renderedPiece[i]!=p){
   lv_label_set_text(pieces[i],letter?t:"");
   lv_obj_center(pieces[i]);
   lv_obj_set_style_text_color(pieces[i],(p&MCUMAX_BLACK)?lv_color_hex(0x16181F):lv_color_hex(0xFFFFFF),0);
   renderedPiece[i]=p;
  }
  const bool dark=(((i>>3)+(i&7))&1);
  uint32_t bg=dark?0x71926F:0xE9E3C9;
  if(selected==(int)sq)bg=0xE6B448;
  else if(selected>=0&&canMove((uint8_t)selected,sq))bg=0x93B5CD;
  if(renderedColor[i]!=bg){
   lv_obj_set_style_bg_color(cells[i],lv_color_hex(bg),0);
   renderedColor[i]=bg;
  }
 }
 lv_label_set_text(statusLabel,gameOver?"No legal moves - Game over":awaitingAI?"Computer thinking...":blackToMove?"Black to move":"White to move");
 lv_label_set_text(modeLabel,vsAI?"MODE: VS COMPUTER":"MODE: TWO PLAYERS");
 char lvl[40];snprintf(lvl,sizeof(lvl),"LEVEL: %d / 5",level);
 lv_label_set_text(levelLabel,lvl);
 lv_label_set_text(lastLabel,lastText);
}

static void boardPressed(lv_event_t *e){
 uint32_t n=(uint32_t)(uintptr_t)lv_event_get_user_data(e);
 queuedSquare=(int)n;
}
static void actionPressed(lv_event_t *e){
 queuedAction=(int)(uintptr_t)lv_event_get_user_data(e);
}
static lv_obj_t *newButton(lv_obj_t *parent,int x,int y,int w,int h,const char *caption,int action){
 lv_obj_t *b=lv_button_create(parent);
 lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);
 lv_obj_set_style_radius(b,8,0);
 lv_obj_set_style_bg_color(b,lv_color_hex(0x34445A),0);
 lv_obj_add_event_cb(b,actionPressed,LV_EVENT_CLICKED,(void*)(uintptr_t)action);
 lv_obj_t *l=lv_label_create(b);lv_label_set_text(l,caption);lv_obj_center(l);
 return b;
}

static void createUI(){
 for(int i=0;i<64;i++){renderedPiece[i]=255;renderedColor[i]=0xFFFFFFFF;}
 screen=lv_screen_active();
 lv_obj_set_style_bg_color(screen,lv_color_hex(0x111C2A),0);
 lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_t *title=lv_label_create(screen);
 lv_label_set_text(title,"CHESS STATION");
 lv_obj_set_style_text_font(title,&lv_font_montserrat_24,0);
 lv_obj_set_style_text_color(title,lv_color_hex(0xF4D58C),0);
 lv_obj_set_pos(title,476,17);
 for(int i=0;i<64;i++){
  int r=i/8,c=i%8;
  cells[i]=lv_button_create(screen);
  lv_obj_set_pos(cells[i],22+c*54,22+r*54);
  lv_obj_set_size(cells[i],54,54);
  lv_obj_set_style_radius(cells[i],0,0);
  lv_obj_set_style_border_width(cells[i],0,0);
  lv_obj_set_style_shadow_width(cells[i],0,0);
  lv_obj_set_style_pad_all(cells[i],0,0);
  lv_obj_add_event_cb(cells[i],boardPressed,LV_EVENT_CLICKED,(void*)(uintptr_t)(((r<<4)|c)));
  pieces[i]=lv_label_create(cells[i]);
  lv_obj_set_style_text_font(pieces[i],&lv_font_montserrat_28,0);
  lv_obj_center(pieces[i]);
 }
 statusLabel=lv_label_create(screen);
 lv_obj_set_style_text_color(statusLabel,lv_color_hex(0xC3D6EC),0);
 lv_obj_set_pos(statusLabel,476,60);
 modeLabel=lv_label_create(screen);lv_obj_set_pos(modeLabel,476,98);
 lv_obj_set_style_text_color(modeLabel,lv_color_hex(0xFFFFFF),0);
 levelLabel=lv_label_create(screen);lv_obj_set_pos(levelLabel,476,140);
 lv_obj_set_style_text_color(levelLabel,lv_color_hex(0xFFFFFF),0);
 newButton(screen,476,180,145,46,"NEW GAME",1);
 newButton(screen,633,180,145,46,"UNDO",2);
 newButton(screen,476,238,302,46,"TOGGLE 1P / 2P",3);
 newButton(screen,476,296,302,46,"CHANGE DIFFICULTY",4);
 lastLabel=lv_label_create(screen);
 lv_obj_set_width(lastLabel,302);
 lv_label_set_long_mode(lastLabel,LV_LABEL_LONG_WRAP);
 lv_obj_set_style_text_color(lastLabel,lv_color_hex(0xF4D58C),0);
 lv_obj_set_pos(lastLabel,476,364);
 lv_obj_t *foot=lv_label_create(screen);
 lv_label_set_text(foot,"mcu-max  |  Waveshare ESP32-S3");
 lv_obj_set_style_text_color(foot,lv_color_hex(0x8EA0B6),0);
 lv_obj_set_pos(foot,476,443);
 drawBoard();
}

static void resetGame(){
 mcumax_init();
 historyCount=0;selected=-1;awaitingAI=false;
 strcpy(lastText,"Tap a piece to begin");
 refreshLegal();
}
static void undoMove(){
 if(historyCount<=0)return;
 int undo=vsAI?(historyCount>=2?2:1):1;
 historyCount-=undo;
 mcumax_init();
 for(int i=0;i<historyCount;i++)mcumax_play_move(history[i]);
 selected=-1;awaitingAI=false;
 strcpy(lastText,"Move undone");
 refreshLegal();
}
static void squareClicked(uint8_t sq){
 Serial.printf("Chess: touch square=%u selected=%d\\n",sq,selected);
 if(gameOver||awaitingAI||(vsAI&&mcumax_get_current_side()==0x10))return;
 mcumax_piece p=mcumax_get_piece(sq);
 bool own=pieceLetter(p)&&((p&MCUMAX_BLACK)==(mcumax_get_current_side()==0x10?MCUMAX_BLACK:0));
 if(selected>=0&&canMove((uint8_t)selected,sq)){
  mcumax_move m={(uint8_t)selected,sq};
  if(mcumax_play_move(m)){
   if(historyCount<512)history[historyCount++]=m;
   moveText(m,false);selected=-1;refreshLegal();
   awaitingAI=vsAI&&!gameOver&&mcumax_get_current_side()==0x10;
   return;
  }
 }
 selected=own&&hasFrom(sq)?sq:-1;
 Serial.printf("Chess: selection=%d\\n",selected);
}
void setup(){
 Serial.begin(115200);
 Board *board=new Board();
 if(!board||!board->init()){Serial.println("Board init failed");while(true)delay(1000);}
 auto rotation=ESP_LV_ADAPTER_ROTATE_0;
 auto tear=ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB;
 LCD *lcd=board->getLCD();
 if(!lcd){Serial.println("No LCD");while(true)delay(1000);}
 auto *bus=lcd->getBus();
 if(bus->getBasicAttributes().type==ESP_PANEL_BUS_TYPE_RGB){
  lcd->configFrameBufferNumber(esp_lv_adapter_get_required_frame_buffer_count(tear,rotation));
  static_cast<BusRGB*>(bus)->configRGB_BounceBufferSize(lcd->getFrameWidth()*10);
 }
 assert(board->begin());
 esp_lv_adapter_config_t cfg=ESP_LV_ADAPTER_DEFAULT_CONFIG();
 cfg.task_stack_size=12*1024;
 cfg.task_priority=2;
 cfg.task_core_id=ARDUINO_RUNNING_CORE;
 ESP_ERROR_CHECK(esp_lv_adapter_init(&cfg));
 esp_lv_adapter_display_config_t displayCfg=ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(lcd,lcd->getFrameWidth(),lcd->getFrameHeight(),rotation);
 displayCfg.profile.use_psram=true;
 lv_display_t *disp=esp_lv_adapter_register_display(&displayCfg);
 assert(disp);
 if(board->getTouch()){
  esp_lv_adapter_touch_config_t touchCfg=ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp,board->getTouch());
  assert(esp_lv_adapter_register_touch(&touchCfg));
 }
 ESP_ERROR_CHECK(esp_lv_adapter_start());
 mcumax_init();refreshLegal();
 ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));
 createUI();
 esp_lv_adapter_unlock();
 Serial.println("Chess Station ready");
}
void loop(){
 int a=queuedAction;
 int s=queuedSquare;
 if(a)queuedAction=0;
 if(s>=0)queuedSquare=-1;
 bool dirty=false;
 if(a){
  switch(a){
   case 1:resetGame();break;
   case 2:undoMove();break;
   case 3:vsAI=!vsAI;resetGame();break;
   case 4:level=level%5+1;break;
  }
  dirty=true;
 }
 if(s>=0&&s<128&&!(s&0x88)){
  squareClicked((uint8_t)s);dirty=true;
 }
 if(dirty){
  Serial.println("Chess: redraw start");
  if(esp_lv_adapter_lock(1000) == ESP_OK){drawBoard();esp_lv_adapter_unlock();Serial.println("Chess: redraw done");}
  else Serial.println("Chess: LVGL lock timeout");
 }
 if(awaitingAI){
  delay(60); // Let LVGL paint the thinking indicator before search begins.
  mcumax_move best=mcumax_search_best_move(limits[level-1],depths[level-1]);
  if(best.from!=MCUMAX_SQUARE_INVALID&&mcumax_play_move(best)){
   if(historyCount<512)history[historyCount++]=best;
   moveText(best,true);
  }else strcpy(lastText,"Computer could not find a move");
  awaitingAI=false;selected=-1;refreshLegal();
  if(esp_lv_adapter_lock(-1) == ESP_OK){drawBoard();esp_lv_adapter_unlock();}
 }
 delay(12);
}
