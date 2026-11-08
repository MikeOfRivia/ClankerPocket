#pragma once
// Self-contained games for Board Station. The chess engine remains in ChessStation.ino.
// Caller owns the LVGL adapter lock while invoking any arcade UI function.
#include <Arduino.h>
#include <lvgl.h>
#include <string.h>

namespace arcade {
enum Game : uint8_t { NONE=0, CHECKERS=1, CONNECT4=2, HANGBOT=3 };
static Game game=NONE;
static bool solo=true;
static uint8_t turn=1;        // 1 = human/white, 2 = black/second player.
static bool done=false;
static char message[96]="";
static lv_obj_t *screen=nullptr;
static lv_obj_t *boardButtons[64]={};
static volatile int input=-1;
static int chosen=-1, chain=-1;
static int8_t checkers[64]={};
static uint8_t connect[42]={};
static uint32_t guesses=0;
static uint8_t misses=0;
static const char *word=nullptr;
static const char *words[]={
 "CIRCUIT","ROBOT","PUZZLE","GALAXY","VECTOR","CHESS","LASER","ENGINE",
 "MYSTERY","BINARY","PIRATE","TURBINE","FUSION","PLANET","GADGET",
 "WIDGET","SPROCKET","PHOTON","ORBIT","MAGNET","DRAGON","PIXEL",
 "CIPHER","SATELLITE","QUANTUM","VOLCANO","JIGSAW","CAPTAIN"
};
static void onButton(lv_event_t *e){
 input=(int)(intptr_t)lv_event_get_user_data(e);
}
static lv_obj_t *button(lv_obj_t *parent,int x,int y,int w,int h,const char *caption,int id,
                        bool enabled=true,int font=0){
 lv_obj_t *b=lv_button_create(parent);
 lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);
 lv_obj_set_style_radius(b,5,0);
 lv_obj_set_style_bg_color(b,lv_color_hex(enabled?0x102C1C:0x102018),0);
 lv_obj_set_style_border_color(b,lv_color_hex(enabled?0x4FEB7C:0x2A5239),0);
 lv_obj_set_style_border_width(b,1,0);
 lv_obj_set_style_shadow_width(b,0,0);
 lv_obj_set_style_pad_all(b,0,0);
 if(enabled)lv_obj_add_event_cb(b,onButton,LV_EVENT_CLICKED,(void*)(intptr_t)id);
 else lv_obj_remove_flag(b,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_t *l=lv_label_create(b);
 lv_label_set_text(l,caption);
 lv_obj_set_style_text_color(l,lv_color_hex(enabled?0xCBFFDB:0x52735A),0);
 if(font==1)lv_obj_set_style_text_font(l,&lv_font_montserrat_24,0);
 lv_obj_center(l);
 return b;
}
static void label(int x,int y,const char *text,int size=0){
 lv_obj_t *l=lv_label_create(screen);
 lv_label_set_text(l,text);
 lv_obj_set_style_text_color(l,lv_color_hex(0xA6FFC0),0);
 lv_obj_set_pos(l,x,y);
 if(size==1)lv_obj_set_style_text_font(l,&lv_font_montserrat_24,0);
}
static void header(const char *title){
 screen=lv_screen_active();
 lv_obj_clean(screen);
 lv_obj_set_style_bg_color(screen,lv_color_hex(0x06120B),0);
 lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
 label(26,16,title,1);
 label(26,58,message);
 button(screen,620,12,154,49,"HOME",900);
 button(screen,620,72,154,49,"NEW GAME",901);
}
static int checkerSide(int p){return p>0?1:p<0?2:0;}
static bool darkSquare(int i){return ((i/8+i%8)&1)!=0;}
static bool checkerHasJumpFrom(int i){
 if(i<0||i>=64||checkerSide(checkers[i])!=turn)return false;
 int r=i/8,c=i%8,p=checkers[i];
 for(int dr=-1;dr<=1;dr+=2){
  if(abs(p)!=2&&dr!=(turn==1?-1:1))continue;
  for(int dc=-1;dc<=1;dc+=2){
   int mr=r+dr,mc=c+dc,nr=r+dr*2,nc=c+dc*2;
   if(nr<0||nr>7||nc<0||nc>7||mr<0||mr>7||mc<0||mc>7)continue;
   if(checkerSide(checkers[mr*8+mc])==3-turn&&checkers[nr*8+nc]==0)return true;
  }
 }
 return false;
}
static bool checkerAnyJump(){
 for(int i=0;i<64;i++)if(checkerHasJumpFrom(i))return true;
 return false;
}
static bool checkerCanMove(int from,int to,bool *jump=nullptr){
 if(from<0||from>=64||to<0||to>=64||!darkSquare(to)||checkers[to])return false;
 int p=checkers[from];
 if(checkerSide(p)!=turn||chain>=0&&chain!=from)return false;
 int r=from/8,c=from%8,rr=to/8,cc=to%8,dr=rr-r,dc=cc-c;
 if(abs(dr)!=abs(dc))return false;
 if(abs(p)!=2&&dr*(turn==1?-1:1)<=0)return false;
 if(abs(dr)==2){
  if(checkerSide(checkers[(r+rr)/2*8+(c+cc)/2])!=3-turn)return false;
  if(jump)*jump=true;
  return true;
 }
 if(abs(dr)==1&&chain<0&&!checkerAnyJump()){
  if(jump)*jump=false;
  return true;
 }
 return false;
}
static bool checkerAnyMove(){
 for(int f=0;f<64;f++)if(checkerSide(checkers[f])==turn)
  for(int t=0;t<64;t++)if(checkerCanMove(f,t))return true;
 return false;
}
static void checkerFinishTurn(){
 chain=-1;chosen=-1;turn=3-turn;
 if(!checkerAnyMove()){
  done=true;
  snprintf(message,sizeof(message),"%s WINS: no legal moves remain",turn==1?"BLACK":"WHITE");
 }else snprintf(message,sizeof(message),"%s TO MOVE",turn==1?"WHITE":"BLACK");
}
static void checkerMove(int from,int to){
 bool jump=false;
 if(!checkerCanMove(from,to,&jump))return;
 int p=checkers[from],r=to/8;
 checkers[from]=0;
 checkers[to]=p;
 if(jump)checkers[((from/8+r)/2)*8+((from%8+to%8)/2)]=0;
 bool promoted=(abs(p)==1&&((turn==1&&r==0)||(turn==2&&r==7)));
 if(promoted)checkers[to]=(turn==1?2:-2);
 if(jump&&!promoted&&checkerHasJumpFrom(to)){
  chain=to;chosen=to;
  snprintf(message,sizeof(message),"%s: CONTINUE YOUR JUMP",turn==1?"WHITE":"BLACK");
 }else checkerFinishTurn();
}
static void checkerSelect(int i){
 if(done||i<0||i>=64)return;
 if(chain>=0){if(checkerCanMove(chain,i))checkerMove(chain,i);return;}
 if(chosen>=0&&checkerCanMove(chosen,i)){checkerMove(chosen,i);return;}
 if(checkerSide(checkers[i])==turn){chosen=i;strcpy(message,checkerAnyJump()?"CAPTURE IS MANDATORY":"SELECT DESTINATION");}
}
static void checkerAI(){
 for(int safety=0;safety<24&&!done&&turn==2;safety++){
  int src[256],dst[256],count=0;
  for(int f=0;f<64;f++)if(checkerSide(checkers[f])==2)
   for(int t=0;t<64;t++)if(checkerCanMove(f,t)&&count<256){src[count]=f;dst[count]=t;count++;}
  if(!count){done=true;strcpy(message,"WHITE WINS");break;}
  int k=(int)random(count);
  checkerMove(src[k],dst[k]);
 }
}
static void showCheckers(){
 header("CHECKERS");
 for(int i=0;i<64;i++){
  const int r=i/8,c=i%8;
  lv_obj_t *b=lv_obj_create(screen);
  lv_obj_set_pos(b,215+c*50,52+r*50);
  lv_obj_set_size(b,50,50);
  lv_obj_set_style_bg_color(b,lv_color_hex(darkSquare(i)?0x274F35:0xB0C4AD),0);
  lv_obj_set_style_border_width(b,chosen==i?3:0,0);
  lv_obj_set_style_border_color(b,lv_color_hex(0x71FF86),0);
  lv_obj_set_style_radius(b,0,0);
  lv_obj_set_style_pad_all(b,0,0);
  lv_obj_remove_flag(b,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(b,onButton,LV_EVENT_CLICKED,(void*)(intptr_t)(100+i));
  int piece=checkers[i];
  if(piece){
   lv_obj_t *disc=lv_obj_create(b);
   lv_obj_set_pos(disc,6,6);lv_obj_set_size(disc,38,38);
   lv_obj_set_style_radius(disc,LV_RADIUS_CIRCLE,0);
   lv_obj_set_style_pad_all(disc,0,0);
   lv_obj_set_style_border_width(disc,3,0);
   lv_obj_set_style_border_color(disc,lv_color_hex(piece>0?0xFAFDE9:0xD36C76),0);
   lv_obj_set_style_bg_color(disc,lv_color_hex(piece>0?0xA8DDAF:0x58202B),0);
   lv_obj_remove_flag(disc,LV_OBJ_FLAG_CLICKABLE);
   if(abs(piece)==2){
    lv_obj_t *k=lv_label_create(disc);lv_label_set_text(k,"K");
    lv_obj_set_style_text_color(k,lv_color_hex(piece>0?0x173A24:0xFFFFFF),0);
    lv_obj_center(k);
   }
  }
 }
 label(25,185,solo?"YOU: WHITE\nCPU: RED":"WHITE vs RED");
 label(25,260,"Captures mandatory.\nMulti-jumps enforced.");
}
static bool c4Win(int side){
 for(int r=0;r<6;r++)for(int c=0;c<7;c++)if(connect[r*7+c]==side){
  const int dr[4]={0,1,1,1},dc[4]={1,0,1,-1};
  for(int d=0;d<4;d++){
   int rr=r+3*dr[d],cc=c+3*dc[d];
   if(rr<0||rr>=6||cc<0||cc>=7)continue;
   bool win=true;
   for(int k=1;k<=3;k++)if(connect[(r+k*dr[d])*7+c+k*dc[d]]!=side)win=false;
   if(win)return true;
  }
 }
 return false;
}
static int c4Row(int c){
 if(c<0||c>=7)return -1;
 for(int r=5;r>=0;r--)if(!connect[r*7+c])return r;
 return -1;
}
static void c4Drop(int col){
 int r=c4Row(col);if(r<0||done)return;
 const int moved=turn;
 connect[r*7+col]=moved;
 if(c4Win(moved)){done=true;snprintf(message,sizeof(message),"%s WINS!",moved==1?"YELLOW":"RED");return;}
 int avail=0;for(int c=0;c<7;c++)if(c4Row(c)>=0)avail++;
 if(!avail){done=true;strcpy(message,"DRAW");return;}
 turn=3-turn;
 snprintf(message,sizeof(message),"%s TO MOVE",turn==1?"YELLOW":"RED");
}
static int c4AIMove(){
 for(int player=2;player>=1;player--){
  for(int col=0;col<7;col++){
   int r=c4Row(col);if(r<0)continue;
   connect[r*7+col]=player;
   bool win=c4Win(player);
   connect[r*7+col]=0;
   if(win)return col;
  }
 }
 int best=-1,bestScore=-999;
 for(int col=0;col<7;col++)if(c4Row(col)>=0){
  int score=10-abs(3-col)*2+(int)random(4);
  if(score>bestScore){bestScore=score;best=col;}
 }
 return best;
}
static void showConnect4(){
 header("CONNECT FOUR");
 label(35,175,solo?"YOU: YELLOW\nCPU: RED":"YELLOW vs RED");
 label(35,260,"Tap any column\nto drop a disc.");
 for(int r=0;r<6;r++)for(int c=0;c<7;c++){
  lv_obj_t *sq=lv_obj_create(screen);
  lv_obj_set_pos(sq,240+c*58,67+r*58);lv_obj_set_size(sq,56,56);
  lv_obj_set_style_bg_color(sq,lv_color_hex(0x163E30),0);
  lv_obj_set_style_radius(sq,4,0);lv_obj_set_style_pad_all(sq,0,0);
  lv_obj_set_style_border_width(sq,1,0);
  lv_obj_set_style_border_color(sq,lv_color_hex(0x317E50),0);
  lv_obj_remove_flag(sq,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(sq,onButton,LV_EVENT_CLICKED,(void*)(intptr_t)(200+c));
  lv_obj_t *disc=lv_obj_create(sq);
  lv_obj_set_pos(disc,7,7);lv_obj_set_size(disc,42,42);
  lv_obj_set_style_radius(disc,LV_RADIUS_CIRCLE,0);
  lv_obj_set_style_pad_all(disc,0,0);
  lv_obj_set_style_bg_color(disc,lv_color_hex(connect[r*7+c]==1?0xF2DC70:connect[r*7+c]==2?0xC84F5C:0x07160F),0);
  lv_obj_set_style_border_width(disc,0,0);
  lv_obj_remove_flag(disc,LV_OBJ_FLAG_CLICKABLE);
 }
}
static bool wordSolved(){
 for(const char *s=word;*s;s++)if(!(guesses&(1u<<(*s-'A'))))return false;
 return true;
}
static void hangGuess(int letter){
 if(done||letter<0||letter>=26||(guesses&(1u<<letter)))return;
 guesses|=1u<<letter;
 bool found=false;
 for(const char *s=word;*s;s++)if(*s-'A'==letter)found=true;
 if(!found)misses++;
 if(wordSolved()){done=true;strcpy(message,"WORD SOLVED!");}
 else if(misses>=6){done=true;snprintf(message,sizeof(message),"OUT OF GUESSES! WORD: %s",word);}
 else snprintf(message,sizeof(message),"WRONG GUESSES: %u / 6",misses);
}
static void showHangbot(){
 header("HANGBOT");
 label(45,123,"CRACK THE SECRET WORD",1);
 char shown[64]={0};
 int j=0;
 for(const char *p=word;*p&&j<60;p++){
  shown[j++]=(guesses&(1u<<(*p-'A')))?*p:'_';
  shown[j++]=' ';
 }
 shown[j]=0;
 label(54,215,shown,1);
 char msg[60];snprintf(msg,sizeof(msg),"ROBOT DAMAGE: %u / 6",misses);
 label(54,285,msg,1);
 label(54,339,misses<6?"Guess letters before the robot breaks.":"ROBOT OFFLINE");
 for(int n=0;n<26;n++){
  char text[2]={(char)('A'+n),0};
  bool available=!(guesses&(1u<<n))&&!done;
  button(screen,370+(n%13)*30,177+(n/13)*63,28,51,text,300+n,available);
 }
}
static void render(){
 if(game==CHECKERS)showCheckers();
 else if(game==CONNECT4)showConnect4();
 else if(game==HANGBOT)showHangbot();
}
static void start(Game g,bool ai){
 game=g;solo=ai;turn=1;done=false;chosen=-1;chain=-1;
 input=-1;guesses=0;misses=0;
 if(g==CHECKERS){
  memset(checkers,0,sizeof(checkers));
  for(int r=0;r<8;r++)for(int c=0;c<8;c++)if(darkSquare(r*8+c)){
   if(r<=2)checkers[r*8+c]=-1;
   if(r>=5)checkers[r*8+c]=1;
  }
  strcpy(message,"WHITE TO MOVE");
 }else if(g==CONNECT4){
  memset(connect,0,sizeof(connect));
  strcpy(message,"YELLOW TO MOVE");
 }else if(g==HANGBOT){
  word=words[random(sizeof(words)/sizeof(words[0]))];
  strcpy(message,"SIX MISSES ALLOWED");
 }
 render();
}
static void handle(int action){
 if(action==901){start(game,solo);return;}
 if(game==CHECKERS&&action>=100&&action<164){
  checkerSelect(action-100);
  if(solo&&!done&&turn==2)checkerAI();
 }else if(game==CONNECT4&&action>=200&&action<207){
  if(!done&&(!solo||turn==1)){
   c4Drop(action-200);
   if(solo&&!done&&turn==2)c4Drop(c4AIMove());
  }
 }else if(game==HANGBOT&&action>=300&&action<326)hangGuess(action-300);
 render();
}
} // namespace arcade
