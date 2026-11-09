#pragma once
// Self-contained games for Board Station. The chess engine remains in ChessStation.ino.
// Caller owns the LVGL adapter lock while invoking any arcade UI function.
#include <Arduino.h>
#include <lvgl.h>
#include <string.h>
extern const lv_image_dsc_t treasure_images[4];

namespace arcade {
enum Game : uint8_t { NONE=0, CHECKERS=1, CONNECT4=2, HANGBOT=3, DOTS=4, MASTERMIND=5, SOS=6 };
static Game game=NONE;
static bool solo=true;
static uint8_t difficulty=1; // Checkers AI: 0 easy, 1 medium, 2 hard.
static uint8_t turn=1;        // 1 = human/white, 2 = black/second player.
static bool done=false;
static char message[96]="";
static lv_obj_t *screen=nullptr;
static lv_obj_t *boardButtons[64]={};
static volatile int input=-1;
static int chosen=-1, chain=-1;
static int8_t checkers[64]={};
static uint8_t connect[42]={};
struct DropAnimation {
 bool active=false;
 volatile bool finished=false;
 int col=0,row=0;
 int player=1;
 lv_obj_t *disc=nullptr;
};
static DropAnimation drop;
static bool cpuDropPending=false;
static uint32_t guesses=0;
static uint8_t misses=0;
static const char *word=nullptr;
// Dots & Boxes: 5x5 dots, 4x4 claimable boxes, 40 edges.
static uint8_t dotsEdges[40]={},dotsBoxes[16]={},dotsScores[2]={};
// Mastermind: six-color code, four positions, ten guesses.
static uint8_t secret[4]={},trial[4]={},mmAttempts=0;
static uint8_t mmHistory[10][4]={},mmExact[10]={},mmPartial[10]={};
static uint8_t mmColor=1;
// SOS: each player places S or O; every SOS scores and earns another turn.
static char sosCells[36]={};
static uint16_t sosScores[2]={};
static char sosLetter='S';

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
 lv_obj_set_style_bg_color(b,lv_color_hex(enabled?0x422A17:0x302014),0);
 lv_obj_set_style_border_color(b,lv_color_hex(enabled?0xC59B58:0x5B4830),0);
 lv_obj_set_style_border_width(b,1,0);
 lv_obj_set_style_shadow_width(b,0,0);
 lv_obj_set_style_pad_all(b,0,0);
 if(enabled)lv_obj_add_event_cb(b,onButton,LV_EVENT_CLICKED,(void*)(intptr_t)id);
 else lv_obj_remove_flag(b,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_t *l=lv_label_create(b);
 lv_label_set_text(l,caption);
 lv_obj_set_style_text_color(l,lv_color_hex(enabled?0xFAE6BD:0x967E55),0);
 if(font==1)lv_obj_set_style_text_font(l,&lv_font_montserrat_24,0);
 lv_obj_center(l);
 return b;
}
static void label(int x,int y,const char *text,int size=0){
 lv_obj_t *l=lv_label_create(screen);
 lv_label_set_text(l,text);
 lv_obj_set_style_text_color(l,lv_color_hex(0xE3C393),0);
 lv_obj_set_pos(l,x,y);
 if(size==1)lv_obj_set_style_text_font(l,&lv_font_montserrat_24,0);
}
static void header(const char *title){
 screen=lv_screen_active();
 lv_obj_clean(screen);
 lv_obj_set_style_bg_color(screen,lv_color_hex(0x21150C),0);
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
// Lightweight minimax over the SAME mandatory-capture/multi-jump rules as
// the human game. Depth and a hard node budget keep touch response reasonable.
struct CheckersSnapshot {
 int8_t board[64];uint8_t turn;int chain,chosen;bool done;char message[96];
};
static CheckersSnapshot checkerSnapshot(){
 CheckersSnapshot s;
 memcpy(s.board,checkers,sizeof(checkers));
 s.turn=turn;s.chain=chain;s.chosen=chosen;s.done=done;
 memcpy(s.message,message,sizeof(message));
 return s;
}
static void checkerRestore(const CheckersSnapshot &s){
 memcpy(checkers,s.board,sizeof(checkers));turn=s.turn;
 chain=s.chain;chosen=s.chosen;done=s.done;
 memcpy(message,s.message,sizeof(message));
}
static int checkerScore(){
 if(done)return turn==1?20000:-20000;
 int score=0;
 for(int i=0;i<64;i++){
  const int p=checkers[i];if(!p)continue;
  int r=i/8,col=i%8;
  int value=(abs(p)==2?175:100);
  if(abs(p)==1)value+=(p<0?r:7-r)*5;
  if(col>=2&&col<=5)value+=7;
  score+=(p<0?value:-value);
 }
 return score;
}
static int checkerSearch(int depth,int alpha,int beta,int &budget){
 if(depth<=0||done||budget<=0)return checkerScore();
 budget--;
 uint8_t sources[128],destinations[128];int count=0;
 for(int i=0;i<64;i++)if(checkerSide(checkers[i])==turn)
  for(int j=0;j<64;j++)if(checkerCanMove(i,j)&&count<128){
   sources[count]=(uint8_t)i;destinations[count]=(uint8_t)j;count++;
  }
 if(!count)return turn==2?-20000:20000;
 bool maxing=(turn==2);
 int best=maxing?-30000:30000;
 for(int k=0;k<count;k++){
  CheckersSnapshot saved=checkerSnapshot();
  checkerMove(sources[k],destinations[k]);
  int value=checkerSearch(depth-1,alpha,beta,budget);
  checkerRestore(saved);
  if(maxing){if(value>best)best=value;if(best>alpha)alpha=best;}
  else {if(value<best)best=value;if(best<beta)beta=best;}
  if(beta<=alpha||budget<=0)break;
 }
 return best;
}
static void checkerAI(){
 // Continue a multi-capture chain until the opponent's turn begins.
 for(int safety=0;safety<24&&!done&&turn==2;safety++){
  uint8_t src[128],dst[128];int count=0;
  for(int f=0;f<64;f++)if(checkerSide(checkers[f])==2)
   for(int t=0;t<64;t++)if(checkerCanMove(f,t)&&count<128){
    src[count]=(uint8_t)f;dst[count]=(uint8_t)t;count++;
   }
  if(!count){done=true;strcpy(message,"WHITE WINS");break;}
  int best=(int)random(count);
  if(difficulty>0){
   const int depth=difficulty==1?2:4;
   int budget=difficulty==1?1000:3600;
   int bestScore=-30000;
   for(int k=0;k<count;k++){
    CheckersSnapshot saved=checkerSnapshot();
    checkerMove(src[k],dst[k]);
    int score=checkerSearch(depth-1,-30000,30000,budget);
    checkerRestore(saved);
    score+=(int)random(3);
    if(score>bestScore){bestScore=score;best=k;}
    if(budget<=0)break;
   }
  }
  checkerMove(src[best],dst[best]);
 }
}
static void showCheckers(){
 header("CHECKERS");
 for(int i=0;i<64;i++){
  const int r=i/8,c=i%8;
  lv_obj_t *b=lv_obj_create(screen);
  lv_obj_set_pos(b,215+c*50,52+r*50);
  lv_obj_set_size(b,50,50);
  lv_obj_set_style_bg_color(b,lv_color_hex(darkSquare(i)?0x765338:0xD8B98E),0);
  lv_obj_set_style_border_width(b,chosen==i?3:0,0);
  lv_obj_set_style_border_color(b,lv_color_hex(0xF5D58D),0);
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
   lv_obj_set_style_border_color(disc,lv_color_hex(piece>0?0xFAEBD0:0xD36C76),0);
   lv_obj_set_style_bg_color(disc,lv_color_hex(piece>0?0xE4D5B8:0x57281E),0);
   lv_obj_remove_flag(disc,LV_OBJ_FLAG_CLICKABLE);
   if(abs(piece)==2){
    lv_obj_t *k=lv_label_create(disc);lv_label_set_text(k,"K");
    lv_obj_set_style_text_color(k,lv_color_hex(piece>0?0x173A24:0xFFFFFF),0);
    lv_obj_center(k);
   }
  }
 }
 label(25,185,solo?"YOU: WHITE\nCPU: RED":"WHITE vs RED");
 if(solo)label(25,235,difficulty==0?"AI: EASY":difficulty==1?"AI: MEDIUM":"AI: HARD");
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
static void c4DiscStyle(lv_obj_t *disc,int player){
 const uint32_t base=player==1?0xDBA72E:0xB83536;
 const uint32_t shine=player==1?0xFFF0A4:0xFF9991;
 lv_obj_set_style_radius(disc,LV_RADIUS_CIRCLE,0);
 lv_obj_set_style_bg_color(disc,lv_color_hex(base),0);
 lv_obj_set_style_bg_grad_color(disc,lv_color_hex(shine),0);
 lv_obj_set_style_bg_grad_dir(disc,LV_GRAD_DIR_VER,0);
 lv_obj_set_style_border_color(disc,lv_color_hex(player==1?0x795113:0x6B1D1B),0);
 lv_obj_set_style_border_width(disc,3,0);
 lv_obj_set_style_shadow_color(disc,lv_color_hex(0x090402),0);
 lv_obj_set_style_shadow_width(disc,5,0);
 lv_obj_set_style_shadow_ofs_y(disc,2,0);
 lv_obj_set_style_pad_all(disc,0,0);
 lv_obj_remove_flag(disc,LV_OBJ_FLAG_CLICKABLE);
}
static void c4Frame(void *obj,int32_t y){
 lv_obj_set_y((lv_obj_t*)obj,y);
}
static void c4Arrived(lv_anim_t *a){(void)a;drop.finished=true;}
static void c4StartDrop(int col){
 if(drop.active||done||col<0||col>6)return;
 int row=c4Row(col);
 if(row<0)return;
 drop.active=true;drop.finished=false;drop.col=col;drop.row=row;drop.player=turn;
 // Disc starts above the cabinet, travels down the center of the chosen chute.
 drop.disc=lv_obj_create(screen);
 lv_obj_set_size(drop.disc,43,43);
 lv_obj_set_pos(drop.disc,240+col*58+7,22);
 c4DiscStyle(drop.disc,turn);
 lv_obj_move_foreground(drop.disc);
 lv_anim_t motion;lv_anim_init(&motion);
 lv_anim_set_var(&motion,drop.disc);
 lv_anim_set_values(&motion,22,67+row*58+7);
 lv_anim_set_duration(&motion,280+row*38);
 lv_anim_set_path_cb(&motion,lv_anim_path_ease_in);
 lv_anim_set_exec_cb(&motion,c4Frame);
 lv_anim_set_completed_cb(&motion,c4Arrived);
 lv_anim_start(&motion);
}
static void showConnect4(){
 header("CONNECT FOUR");
 // Carved cabinet, brass fasteners and inset circular wells.
 label(26,175,solo?"YOU: GOLD\nCPU: RUBY":"GOLD vs RUBY");
 label(26,253,"Tap a column\nto release a disc.");
 lv_obj_t *cabinet=lv_obj_create(screen);
 lv_obj_set_pos(cabinet,226,53);lv_obj_set_size(cabinet,430,374);
 lv_obj_set_style_radius(cabinet,13,0);
 lv_obj_set_style_pad_all(cabinet,0,0);
 lv_obj_set_style_bg_color(cabinet,lv_color_hex(0x4C2915),0);
 lv_obj_set_style_bg_grad_color(cabinet,lv_color_hex(0x986236),0);
 lv_obj_set_style_bg_grad_dir(cabinet,LV_GRAD_DIR_HOR,0);
 lv_obj_set_style_border_color(cabinet,lv_color_hex(0xBC8E4A),0);
 lv_obj_set_style_border_width(cabinet,5,0);
 lv_obj_set_style_shadow_color(cabinet,lv_color_hex(0x090403),0);
 lv_obj_set_style_shadow_width(cabinet,15,0);
 lv_obj_remove_flag(cabinet,LV_OBJ_FLAG_CLICKABLE);
 for(int r=0;r<6;r++)for(int c=0;c<7;c++){
  const int x=240+c*58,y=67+r*58;
  lv_obj_t *sq=lv_obj_create(screen);
  lv_obj_set_pos(sq,x,y);lv_obj_set_size(sq,56,56);
  lv_obj_set_style_bg_color(sq,lv_color_hex(0x26170E),0);
  lv_obj_set_style_radius(sq,LV_RADIUS_CIRCLE,0);
  lv_obj_set_style_pad_all(sq,0,0);
  lv_obj_set_style_border_width(sq,4,0);
  lv_obj_set_style_border_color(sq,lv_color_hex(0xAC7C42),0);
  lv_obj_set_style_shadow_width(sq,3,0);
  lv_obj_set_style_shadow_color(sq,lv_color_hex(0x0C0604),0);
  lv_obj_remove_flag(sq,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(sq,onButton,LV_EVENT_CLICKED,(void*)(intptr_t)(200+c));
  if(connect[r*7+c]){
   lv_obj_t *disc=lv_obj_create(sq);
   lv_obj_set_pos(disc,3,3);lv_obj_set_size(disc,43,43);
   c4DiscStyle(disc,connect[r*7+c]);
  }else{
   lv_obj_t *well=lv_obj_create(sq);
   lv_obj_set_pos(well,5,5);lv_obj_set_size(well,39,39);
   lv_obj_set_style_radius(well,LV_RADIUS_CIRCLE,0);
   lv_obj_set_style_bg_color(well,lv_color_hex(0x100B08),0);
   lv_obj_set_style_border_color(well,lv_color_hex(0x372116),0);
   lv_obj_set_style_border_width(well,3,0);
   lv_obj_set_style_pad_all(well,0,0);
   lv_obj_remove_flag(well,LV_OBJ_FLAG_CLICKABLE);
  }
 }
 for(int i=0;i<4;i++){
  lv_obj_t *rivet=lv_obj_create(screen);
  lv_obj_set_pos(rivet,229+(i%2)*420,57+(i/2)*361);
  lv_obj_set_size(rivet,9,9);
  lv_obj_set_style_radius(rivet,LV_RADIUS_CIRCLE,0);
  lv_obj_set_style_bg_color(rivet,lv_color_hex(0xD7B16D),0);
  lv_obj_set_style_border_width(rivet,1,0);
  lv_obj_set_style_border_color(rivet,lv_color_hex(0x5D381F),0);
  lv_obj_set_style_pad_all(rivet,0,0);
  lv_obj_remove_flag(rivet,LV_OBJ_FLAG_CLICKABLE);
 }
}
static void render();
static bool c4Tick(){
 if(game!=CONNECT4)return false;
 if(drop.active&&!drop.finished)return false;
 if(drop.active&&drop.finished){
  const int col=drop.col;
  drop.active=false;drop.finished=false;drop.disc=nullptr;
  // Move resolves only after the falling animation arrives.
  c4Drop(col);
  if(solo&&!done&&turn==2)cpuDropPending=true;
  render();
  return true;
 }
 if(cpuDropPending&&!drop.active){
  cpuDropPending=false;
  int col=c4AIMove();
  if(col>=0)c4StartDrop(col);
  return true;
 }
 return false;
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
static void bangBotHit(int x,int y,int w,int h,int id,bool active=true){
 lv_obj_t *b=lv_obj_create(screen);
 lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);
 lv_obj_set_style_bg_opa(b,LV_OPA_TRANSP,0);
 lv_obj_set_style_border_width(b,0,0);
 lv_obj_set_style_pad_all(b,0,0);
 lv_obj_set_style_radius(b,0,0);
 lv_obj_remove_flag(b,LV_OBJ_FLAG_SCROLLABLE);
 if(active)lv_obj_add_event_cb(b,onButton,LV_EVENT_CLICKED,(void*)(intptr_t)id);
 else lv_obj_remove_flag(b,LV_OBJ_FLAG_CLICKABLE);
}
static void bangPanel(int x,int y,int w,int h,uint32_t color,uint8_t opacity=LV_OPA_COVER){
 lv_obj_t *b=lv_obj_create(screen);
 lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);
 lv_obj_set_style_bg_color(b,lv_color_hex(color),0);
 lv_obj_set_style_bg_opa(b,opacity,0);
 lv_obj_set_style_border_width(b,0,0);
 lv_obj_set_style_radius(b,2,0);
 lv_obj_set_style_pad_all(b,0,0);
 lv_obj_remove_flag(b,LV_OBJ_FLAG_CLICKABLE);
}
static void showHangbot(){
 screen=lv_screen_active();lv_obj_clean(screen);
 lv_obj_set_style_bg_color(screen,lv_color_hex(0x29170F),0);
 lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_t *bg=lv_image_create(screen);
 lv_image_set_src(bg,&treasure_images[3]);
 lv_obj_set_pos(bg,0,0);
 lv_obj_remove_flag(bg,LV_OBJ_FLAG_CLICKABLE);
 // Opaque label area covers the placeholder answer rendered in the artwork.
 bangPanel(203,252,382,49,0xAE7844);
 char shown[80]={0};int j=0;
 for(const char *p=word;*p&&j<76;p++){
  shown[j++]=(guesses&(1u<<(*p-'A')))?*p:'_';
  shown[j++]=' ';
 }
 lv_obj_t *answer=lv_label_create(screen);
 lv_label_set_text(answer,shown);
 lv_obj_set_style_text_font(answer,&lv_font_montserrat_24,0);
 lv_obj_set_style_text_color(answer,lv_color_hex(0x2D1B0C),0);
 lv_obj_align(answer,LV_ALIGN_TOP_MID,0,260);
 // The supplied illustration includes two demo damage Xs: hide them and
 // render the actual current state, in the specified six-step order.
 bangPanel(723,55,35,201,0x2D1C12);
 static const char *parts[]={"LEFT LEG","RIGHT LEG","LEFT ARM","RIGHT ARM","TORSO","HEAD"};
 static const int px[]={340,434,304,483,392,409};
 static const int py[]={222,222,166,170,155,83};
 for(int i=0;i<6;i++){
  if(i<misses){
   lv_obj_t *status=lv_label_create(screen);
   lv_label_set_text(status,"X");
   lv_obj_set_style_text_font(status,&lv_font_montserrat_24,0);
   lv_obj_set_style_text_color(status,lv_color_hex(0xEE4336),0);
   lv_obj_set_pos(status,727,55+i*34);
   lv_obj_t *damage=lv_label_create(screen);
   lv_label_set_text(damage,"X");
   lv_obj_set_style_text_font(damage,&lv_font_montserrat_28,0);
   lv_obj_set_style_text_color(damage,lv_color_hex(0xF94A25),0);
   lv_obj_set_pos(damage,px[i]-12,py[i]-17);
   lv_obj_remove_flag(damage,LV_OBJ_FLAG_CLICKABLE);
  }
 }
 if(misses>0){
  bangPanel(68,95,182,79,0x392416);
  char impact[48];
  snprintf(impact,sizeof(impact),"%s HIT!",parts[misses-1]);
  lv_obj_t *impactLabel=lv_label_create(screen);
  lv_label_set_text(impactLabel,impact);
  lv_obj_set_style_text_color(impactLabel,lv_color_hex(0xFFCC86),0);
  lv_obj_set_pos(impactLabel,82,122);
 }
 if(done){
  bangPanel(210,250,374,52,0x986237);
  lv_label_set_text(answer,wordSolved()?"ROBOT SURVIVED!":word);
  lv_obj_align(answer,LV_ALIGN_TOP_MID,0,260);
 }
 const int xs[3]={143,144,241};
 const int ys[3]={313,354,396};
 for(int n=0;n<26;n++){
  int row=n<10?0:n<20?1:2;
  int col=row==0?n:row==1?n-10:n-20;
  int x=xs[row]+col*51;
  bool enabled=!(guesses&(1u<<n))&&!done;
  if(!enabled)bangPanel(x,ys[row],44,35,0x20140E,LV_OPA_60);
  bangBotHit(x,ys[row],44,34,300+n,enabled);
 }
 bangBotHit(37,393,91,62,900);
 bangBotHit(658,387,111,70,901);
}
static int dotsEdgeCount(){return 40;}
static int dotsBoxEdge(int row,int col,int side){
 // Horizontal edges 0..19 (5 rows x 4 columns).
 // Vertical edges 20..39 (4 rows x 5 columns).
 return side==0?row*4+col:
        side==1?20+row*5+col+1:
        side==2?(row+1)*4+col:
                 20+row*5+col;
}
static int dotsMove(int e){
 if(e<0||e>=40||dotsEdges[e]||done)return 0;
 dotsEdges[e]=turn;
 int gained=0;
 for(int r=0;r<4;r++)for(int c=0;c<4;c++){
  int k=r*4+c;
  if(dotsBoxes[k])continue;
  if(dotsEdges[dotsBoxEdge(r,c,0)]&&dotsEdges[dotsBoxEdge(r,c,1)]&&
     dotsEdges[dotsBoxEdge(r,c,2)]&&dotsEdges[dotsBoxEdge(r,c,3)]){
   dotsBoxes[k]=turn;gained++;dotsScores[turn-1]++;
  }
 }
 int filled=0;for(int i=0;i<40;i++)if(dotsEdges[i])filled++;
 if(filled==40){
  done=true;
  snprintf(message,sizeof(message),"%s",dotsScores[0]==dotsScores[1]?"DRAW":dotsScores[0]>dotsScores[1]?"WHITE WINS":"RED WINS");
 }else{
  if(!gained)turn=3-turn;
  snprintf(message,sizeof(message),"WHITE %u  RED %u  |  %s",
   dotsScores[0],dotsScores[1],turn==1?"WHITE TO MOVE":"RED TO MOVE");
 }
 return gained;
}
static void showDots(){
 header("DOTS & BOXES");
 label(25,174,solo?"YOU: WHITE\nCPU: RED":"WHITE vs RED");
 label(25,265,"Claim a box to\nplay again.");
 const int ox=250,oy=94,step=76;
 for(int r=0;r<5;r++)for(int c=0;c<5;c++){
  lv_obj_t *dot=lv_obj_create(screen);
  lv_obj_set_pos(dot,ox+c*step-5,oy+r*step-5);
  lv_obj_set_size(dot,11,11);
  lv_obj_set_style_radius(dot,LV_RADIUS_CIRCLE,0);
  lv_obj_set_style_border_width(dot,0,0);
  lv_obj_set_style_pad_all(dot,0,0);
  lv_obj_set_style_bg_color(dot,lv_color_hex(0xF5D9A4),0);
  lv_obj_remove_flag(dot,LV_OBJ_FLAG_CLICKABLE);
 }
 for(int e=0;e<40;e++){
  const bool horizontal=e<20;
  const int row=horizontal?(e/4):((e-20)/5);
  const int col=horizontal?(e%4):((e-20)%5);
  const int x=ox+col*step+(horizontal?9:-5);
  const int y=oy+row*step+(horizontal?-7:9);
  lv_obj_t *b=lv_obj_create(screen);
  lv_obj_set_pos(b,x,y);
  lv_obj_set_size(b,horizontal?step-18:11,horizontal?13:step-18);
  lv_obj_set_style_radius(b,3,0);
  lv_obj_set_style_pad_all(b,0,0);
  lv_obj_set_style_border_width(b,0,0);
  lv_obj_set_style_bg_color(b,lv_color_hex(dotsEdges[e]==1?0xF8E6BA:dotsEdges[e]==2?0xF36677:0x53371F),0);
  if(!dotsEdges[e])lv_obj_add_event_cb(b,onButton,LV_EVENT_CLICKED,(void*)(intptr_t)(400+e));
  else lv_obj_remove_flag(b,LV_OBJ_FLAG_CLICKABLE);
 }
 for(int r=0;r<4;r++)for(int c=0;c<4;c++)if(dotsBoxes[r*4+c]){
  lv_obj_t *lab=lv_label_create(screen);
  lv_label_set_text(lab,dotsBoxes[r*4+c]==1?"W":"R");
  lv_obj_set_style_text_color(lab,lv_color_hex(dotsBoxes[r*4+c]==1?0xF8E6BA:0xF36677),0);
  lv_obj_set_pos(lab,ox+c*step+33,oy+r*step+27);
 }
}
static int mmCheck(){
 uint8_t exact=0,partial=0;
 bool usedSecret[4]={},usedGuess[4]={};
 for(int i=0;i<4;i++)if(trial[i]==secret[i]){exact++;usedSecret[i]=usedGuess[i]=true;}
 for(int i=0;i<4;i++)if(!usedGuess[i]){
  for(int k=0;k<4;k++)if(!usedSecret[k]&&trial[i]==secret[k]){partial++;usedSecret[k]=true;break;}
 }
 if(mmAttempts<10){
  for(int i=0;i<4;i++)mmHistory[mmAttempts][i]=trial[i];
  mmExact[mmAttempts]=exact;mmPartial[mmAttempts]=partial;
 }
 mmAttempts++;
 if(exact==4){done=true;strcpy(message,"CODE CRACKED!");}
 else if(mmAttempts>=10){
  done=true;
  snprintf(message,sizeof(message),"FAILED! CODE: %u %u %u %u",secret[0],secret[1],secret[2],secret[3]);
 }else snprintf(message,sizeof(message),"EXACT %u  MISPLACED %u",exact,partial);
 for(int i=0;i<4;i++)trial[i]=0;
 return exact;
}
static void showMastermind(){
 header("MASTERMIND");
 label(24,130,"Crack the 4-color code.");
 label(24,163,"Green = correct place.\nAmber = wrong place.");
 char line[80];
 for(int row=0;row<mmAttempts&&row<10;row++){
  snprintf(line,sizeof(line),"%02u: %u %u %u %u    EXACT %u / MISPLACED %u",
    (unsigned)(row+1),mmHistory[row][0],mmHistory[row][1],
    mmHistory[row][2],mmHistory[row][3],mmExact[row],mmPartial[row]);
  label(250,75+row*31,line);
 }
 for(int i=0;i<4;i++){
  char number[4];snprintf(number,sizeof(number),"%u",trial[i]);
  button(screen,270+i*95,402,78,58,number,510+i,!done,1);
 }
 for(int i=1;i<=6;i++){
  char number[4];snprintf(number,sizeof(number),"%u",i);
  button(screen,32+(i-1)*64,297,55,60,number,500+i,!done,1);
 }
 label(34,365,"Select a color, then tap a slot.");
 button(screen,661,399,111,61,"GUESS",520,!done);
}
static int sosCount(int at){
 int r=at/6,c=at%6,amount=0;
 const int dy[4]={0,1,1,-1},dx[4]={1,0,1,1};
 // Evaluate triples containing the just-placed tile in any of three slots.
 for(int d=0;d<4;d++)for(int offset=-2;offset<=0;offset++){
  const int r0=r+offset*dy[d],c0=c+offset*dx[d];
  const int r2=r0+2*dy[d],c2=c0+2*dx[d];
  if(r0<0||r2<0||c0<0||c2<0||r0>=6||r2>=6||c0>=6||c2>=6)continue;
  if(sosCells[r0*6+c0]=='S'&&sosCells[(r0+dy[d])*6+c0+dx[d]]=='O'&&
     sosCells[r2*6+c2]=='S')amount++;
 }
 return amount;
}
static void sosPlace(int pos,char letter){
 if(pos<0||pos>=36||sosCells[pos]||done)return;
 sosCells[pos]=letter;
 int gained=sosCount(pos);
 sosScores[turn-1]+=gained;
 int occupied=0;for(int i=0;i<36;i++)if(sosCells[i])occupied++;
 if(occupied==36){
  done=true;
  strcpy(message,sosScores[0]==sosScores[1]?"DRAW":sosScores[0]>sosScores[1]?"WHITE WINS":"RED WINS");
 }else{
  if(!gained)turn=3-turn;
  snprintf(message,sizeof(message),"WHITE %u  RED %u  |  %s",
    sosScores[0],sosScores[1],turn==1?"WHITE TO MOVE":"RED TO MOVE");
 }
}
static void showSOS(){
 header("SOS");
 label(32,185,solo?"YOU: WHITE\nCPU: RED":"WHITE vs RED");
 label(32,265,"Complete SOS in\nany direction.");
 button(screen,35,354,80,60,"S",650,!done,1);
 button(screen,129,354,80,60,"O",651,!done,1);
 label(34,426,sosLetter=='S'?"SELECTED: S":"SELECTED: O");
 for(int i=0;i<36;i++){
  const int r=i/6,c=i%6;
  char name[2]={sosCells[i],0};
  if(!name[0])strcpy(name," ");
  lv_obj_t *b=button(screen,259+c*68,63+r*68,64,64,name,600+i,!done,1);
  if(sosCells[i])lv_obj_remove_flag(b,LV_OBJ_FLAG_CLICKABLE);
 }
}
static void dotsAI(){
 // Avoid gifting boxes when possible, with an immediate box-claim priority.
 for(int safety=0;safety<45&&!done&&turn==2;safety++){
  int choice=-1,best=-10000;
  for(int e=0;e<40;e++)if(!dotsEdges[e]){
   int score=(int)random(10);
   for(int r=0;r<4;r++)for(int c=0;c<4;c++){
    bool contains=false;int touched=0;
    for(int side=0;side<4;side++){
     int edge=dotsBoxEdge(r,c,side);
     if(edge==e)contains=true;
     if(dotsEdges[edge])touched++;
    }
    if(contains&&touched==3)score+=100;
    else if(contains&&touched==2)score-=20;
   }
   if(score>best){best=score;choice=e;}
  }
  if(choice<0)break;
  dotsMove(choice);
 }
}
static void sosAI(){
 for(int safety=0;safety<40&&!done&&turn==2;safety++){
  int best=-1,bestCount=-1;
  char bestLetter='S';
  for(int i=0;i<36;i++)if(!sosCells[i]){
   for(char letter='O';letter<='S';letter+=4){
    sosCells[i]=letter;
    int score=sosCount(i);
    sosCells[i]=0;
    if(score>bestCount){best=i;bestCount=score;bestLetter=letter;}
   }
  }
  if(best<0)break;
  sosPlace(best,bestLetter);
 }
}

static void render(){
 if(game==CHECKERS)showCheckers();
 else if(game==CONNECT4)showConnect4();
 else if(game==HANGBOT)showHangbot();
 else if(game==DOTS)showDots();
 else if(game==MASTERMIND)showMastermind();
 else if(game==SOS)showSOS();
}
static void start(Game g,bool ai){
 drop.active=false;drop.finished=false;drop.disc=nullptr;cpuDropPending=false;
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
 }else if(g==DOTS){
  memset(dotsEdges,0,sizeof(dotsEdges));memset(dotsBoxes,0,sizeof(dotsBoxes));
  memset(dotsScores,0,sizeof(dotsScores));strcpy(message,"WHITE TO MOVE");
 }else if(g==MASTERMIND){
  mmAttempts=0;mmColor=1;memset(trial,0,sizeof(trial));
  for(int i=0;i<4;i++)secret[i]=(uint8_t)random(1,7);
  strcpy(message,"TEN GUESSES TO CRACK THE CODE");
 }else if(g==SOS){
  memset(sosCells,0,sizeof(sosCells));sosScores[0]=sosScores[1]=0;
  sosLetter='S';strcpy(message,"WHITE TO MOVE");
 }
 render();
}
static void handle(int action){
 if(action==901){start(game,solo);return;}
 if(game==CHECKERS&&action>=100&&action<164){
  checkerSelect(action-100);
  if(solo&&!done&&turn==2)checkerAI();
 }else if(game==CONNECT4&&action>=200&&action<207){
  if(!done&&!drop.active&&(!solo||turn==1))c4StartDrop(action-200);
  return; // Keep the board alive while the drop animation runs.
 }else if(game==HANGBOT&&action>=300&&action<326)hangGuess(action-300);
 else if(game==DOTS&&action>=400&&action<440){
  if(!solo||turn==1){dotsMove(action-400);if(solo&&!done&&turn==2)dotsAI();}
 }else if(game==MASTERMIND&&!done){
  if(action>=501&&action<=506)mmColor=(uint8_t)(action-500);
  else if(action>=510&&action<514)trial[action-510]=mmColor;
  else if(action==520){
   bool ready=true;for(int i=0;i<4;i++)if(!trial[i])ready=false;
   if(ready)mmCheck();else strcpy(message,"FILL ALL FOUR SLOTS FIRST");
  }
 }else if(game==SOS&&!done){
  if(action==650)sosLetter='S';
  else if(action==651)sosLetter='O';
  else if(action>=600&&action<636&&(!solo||turn==1)){
   sosPlace(action-600,sosLetter);
   if(solo&&!done&&turn==2)sosAI();
  }
 }
 render();
}
} // namespace arcade
