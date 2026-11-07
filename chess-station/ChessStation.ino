#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <esp_err.h>
#include <lvgl.h>
#include "esp_lv_adapter_arduino.h"
#include "mcu-max.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

static lv_obj_t *cells[64], *pieces[64], *moveDots[64], *statusLabel, *modeLabel, *levelLabel, *lastLabel, *undoButton;
static bool inGame=false;
enum GameVariant : uint8_t { VARIANT_STANDARD=0, VARIANT_KNIGHTS=1, VARIANT_PAWNS=2, VARIANT_SPY=3 };
static GameVariant variant=VARIANT_STANDARD;
static lv_obj_t *modeOverlay=nullptr, *endOverlay=nullptr;
enum EndReason : uint8_t { END_NONE=0, END_CHECKMATE, END_STALEMATE, END_INFILTRATION, END_EXPOSED };
static EndReason endReason=END_NONE;
static uint8_t spyChecks[2]={0,0}; // White and Black checks received.
static int8_t winnerSide=-1; // 0 white, 1 black; -1 draw or unfinished.
static bool endModalShown=false;
static lv_obj_t *turnFrame[2]={nullptr,nullptr};
static lv_obj_t *spyPips[2][3]={{nullptr,nullptr,nullptr},{nullptr,nullptr,nullptr}};
static uint8_t renderedSpyChecks[2]={255,255};
static int8_t renderedTurn=-1;
static const char *variantName(){return variant==VARIANT_KNIGHTS?"KNIGHTS ONLY":variant==VARIANT_PAWNS?"PAWNS ONLY":variant==VARIANT_SPY?"SPY KINGS":"STANDARD";}
static lv_obj_t *screen;
static volatile int queuedSquare=-1;
static volatile int queuedAction=0;
static bool vsAI=true, awaitingAI=false, gameOver=false;
static uint8_t level=3; // Intermediate, intended as the user's starting difficulty
static int selected=-1;
static mcumax_move validMoves[256];
static uint32_t validCount=0;
static mcumax_move history[512];
static int historyCount=0;
static char lastText[56]="Tap a piece to begin";
static uint8_t renderedPiece[64];
static uint32_t renderedColor[64];
static uint8_t renderedDot[64];
extern const lv_image_dsc_t chess_piece_images[24];
extern const lv_image_dsc_t chess_logo;
extern const lv_image_dsc_t chess_rotated_buttons[2];
// Level 1-3 are deliberately weakened: shallow search alone is still very
// unforgiving at beginner ratings. No Elo claim without actual calibration.
// Level 4 retains the strongest search budget from the previous 5-level build.
static const char *levelNames[]={"BEGINNER","NOVICE","INTERMEDIATE","MAX ENGINE"};
static const uint32_t limits[]={0,250,500,45000};
static const uint8_t depths[]={0,2,3,6};
// Chance that weaker settings choose a simple legal move instead of engine best.
static const uint8_t nonOptimalPercent[]={100,60,25,0};


static bool hasFrom(uint8_t sq){for(uint32_t i=0;i<validCount&&i<256;i++)if(validMoves[i].from==sq)return true;return false;}
static bool canMove(uint8_t from,uint8_t to){for(uint32_t i=0;i<validCount&&i<256;i++)if(validMoves[i].from==from&&validMoves[i].to==to)return true;return false;}

static int checkedKingSquare();
static void refreshLegal(){
 validCount=mcumax_search_valid_moves(validMoves,256);
 if(endReason!=END_NONE){gameOver=true;return;}
 if(validCount==0){
  gameOver=true;
  endReason=(checkedKingSquare()>=0)?END_CHECKMATE:END_STALEMATE;
  winnerSide=(endReason==END_CHECKMATE&&(vsAI||variant!=VARIANT_SPY))?
    ((mcumax_get_current_side()==0x10)?0:1):-1;
 }else gameOver=false;
}
static void resolveSpyMove(mcumax_move m){
 if(vsAI||variant!=VARIANT_SPY)return;
 const bool moverBlack=(mcumax_get_current_side()!=0x10);
 const mcumax_piece arrived=mcumax_get_piece(m.to);
 if((arrived&7)==MCUMAX_KING &&
    ((moverBlack&&(m.to>>4)==7)||(!moverBlack&&(m.to>>4)==0))){
  endReason=END_INFILTRATION;
  winnerSide=moverBlack?1:0;
  return;
 }
 // Checkmate is a draw in Spy Kings. Do not count a mating check as
 // the third exposure, otherwise the two end conditions would conflict.
 // mcu-max supplies the legal reply count for the side now to move.
 if(mcumax_search_valid_moves(validMoves,256)==0)return;
 // Count one exposure for each legal move that gives check.
 if(checkedKingSquare()>=0){
  const int exposed=(mcumax_get_current_side()==0x10)?1:0;
  if(spyChecks[exposed]<3)spyChecks[exposed]++;
  if(spyChecks[exposed]>=3){
   endReason=END_EXPOSED;
   winnerSide=exposed?0:1;
  }
 }
}

// A lightweight, varied opponent for the lower settings. Legal moves come
// from mcu-max, so even the beginner never makes an illegal move.
static mcumax_move selectComputerMove(){
 if(validCount==0)return {MCUMAX_SQUARE_INVALID,MCUMAX_SQUARE_INVALID};
 const int count=(validCount>256)?256:(int)validCount;
 if(random(100)<nonOptimalPercent[level-1]){
  // Beginners recognize some captures but also miss opportunities.
  uint16_t weights[256];
  uint32_t total=0;
  for(int i=0;i<count;i++){
   mcumax_piece victim=mcumax_get_piece(validMoves[i].to);
   // Captures are attractive but not always chosen.
   weights[i]=1+((victim&7)?3:0);
   total+=weights[i];
  }
  uint32_t roll=(uint32_t)random((long)total);
  for(int i=0;i<count;i++){
   if(roll<weights[i])return validMoves[i];
   roll-=weights[i];
  }
  return validMoves[count-1];
 }
 return mcumax_search_best_move(limits[level-1],depths[level-1]);
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
// Map engine pieces to our 12 pre-rendered glyph sprites (white, then black).
static int pieceSpriteIndex(mcumax_piece p){
 int offset=(p&MCUMAX_BLACK)?6:0;
 switch(p&7){
  case MCUMAX_KING:return offset+0;
  case MCUMAX_QUEEN:return offset+1;
  case MCUMAX_ROOK:return offset+2;
  case MCUMAX_BISHOP:return offset+3;
  case MCUMAX_KNIGHT:return offset+4;
  case MCUMAX_PAWN_UPSTREAM: case MCUMAX_PAWN_DOWNSTREAM:return offset+5;
  default:return -1;
 }
}
static void squareName(uint8_t s,char out[3]){
 out[0]='a'+(s&7);out[1]='8'-(s>>4);out[2]=0;
}
static void moveText(mcumax_move m,bool ai){
 char a[3],b[3];squareName(m.from,a);squareName(m.to,b);
 snprintf(lastText,sizeof(lastText),"%s: %s to %s",ai?"Computer":"Player",a,b);
}


// Detect whether the side to move is in check. mcu-max exposes legal moves,
// but not an explicit in-check query. Board uses 0x88 indexing (rank*16+file).
static bool boardSquare(int s){return s>=0&&s<128&&!(s&0x88);}
static bool enemyPiece(int s,bool enemyBlack,int kind){
 if(!boardSquare(s))return false;
 const mcumax_piece p=mcumax_get_piece((uint8_t)s);
 return (p&7)==kind&&!!(p&MCUMAX_BLACK)==enemyBlack;
}
static int checkedKingSquare(){
 const bool usBlack=(mcumax_get_current_side()==0x10);
 int king=-1;
 for(int r=0;r<8&&king<0;r++)for(int f=0;f<8;f++){
  int s=r*16+f;
  const mcumax_piece p=mcumax_get_piece((uint8_t)s);
  if((p&7)==MCUMAX_KING&&!!(p&MCUMAX_BLACK)==usBlack){king=s;break;}
 }
 if(king<0)return -1;
 const bool enemyBlack=!usBlack;
 const int pawnRankOffset=enemyBlack?-16:16;
 for(int df=-1;df<=1;df+=2){
  int attacker=king+pawnRankOffset+df;
  if(enemyPiece(attacker,enemyBlack,enemyBlack?MCUMAX_PAWN_DOWNSTREAM:MCUMAX_PAWN_UPSTREAM))return king;
 }
 static const int knights[]={-33,-31,-18,-14,14,18,31,33};
 for(int d:knights)if(enemyPiece(king+d,enemyBlack,MCUMAX_KNIGHT))return king;
 static const int kingSteps[]={-17,-16,-15,-1,1,15,16,17};
 for(int d:kingSteps)if(enemyPiece(king+d,enemyBlack,MCUMAX_KING))return king;
 static const int diagonal[]={-17,-15,15,17};
 static const int straight[]={-16,-1,1,16};
 for(int d:diagonal){
  for(int q=king+d;boardSquare(q);q+=d){
   mcumax_piece p=mcumax_get_piece((uint8_t)q);
   if(!(p&7))continue;
   if(!!(p&MCUMAX_BLACK)==enemyBlack&&((p&7)==MCUMAX_BISHOP||(p&7)==MCUMAX_QUEEN))return king;
   break;
  }
 }
 for(int d:straight){
  for(int q=king+d;boardSquare(q);q+=d){
   mcumax_piece p=mcumax_get_piece((uint8_t)q);
   if(!(p&7))continue;
   if(!!(p&MCUMAX_BLACK)==enemyBlack&&((p&7)==MCUMAX_ROOK||(p&7)==MCUMAX_QUEEN))return king;
   break;
  }
 }
 return -1;
}

static void drawBoard(){
 const bool blackToMove=(mcumax_get_current_side()==0x10);
 const int checkedKing=checkedKingSquare();
 for(int i=0;i<64;i++){
  uint8_t sq=((i>>3)<<4)|(i&7);
  mcumax_piece p=mcumax_get_piece(sq);

  // Only touch LVGL objects that actually changed. Rebuilding 64 labels and
  // restyling 64 buttons for a single selection caused excessive redraw work.
  if(renderedPiece[i]!=p){
   int sprite=pieceSpriteIndex(p);
   if(sprite>=0){
    lv_image_set_src(pieces[i],&chess_piece_images[sprite+(vsAI?0:12)]);
    lv_obj_remove_flag(pieces[i],LV_OBJ_FLAG_HIDDEN);
   }else{
    lv_obj_add_flag(pieces[i],LV_OBJ_FLAG_HIDDEN);
   }
   renderedPiece[i]=p;
  }
  const bool dark=(((i>>3)+(i&7))&1);
  uint32_t bg=dark?0x526858:0xA5B9A5;
  // Subtle last-move markers on BOTH origin and destination, including the
  // computer's reply. Derived from history so Undo and New Game stay correct.
  if(historyCount>0){
   const mcumax_move last=history[historyCount-1];
   if(sq==last.from||sq==last.to)bg=dark?0x6D9874:0xC0D9B9;
  }
  // Capture squares are red, but the selected piece remains gold.
  const bool legalTarget=selected>=0&&canMove((uint8_t)selected,sq);
  const bool capture=legalTarget&&((p&7)!=MCUMAX_EMPTY);
  if(selected==(int)sq)bg=0x58DF7A;
  else if(capture)bg=0xBE4950;
  // Check highlights only the king. Checkmate highlights a full 3x3
  // cluster around the mated king (clipped at the board edges).
  if(checkedKing>=0){
   const int kr=checkedKing>>4,kf=checkedKing&7;
   const int sr=sq>>4,sf=sq&7;
   if(endReason==END_CHECKMATE&&abs(sr-kr)<=1&&abs(sf-kf)<=1)
    bg=(sq==checkedKing)?0xDC252B:0xC9474B;
   else if(sq==checkedKing)bg=0xDC343F;
  }
  const bool dot=legalTarget&&!capture&&sq!=checkedKing;
  if(renderedDot[i]!=(uint8_t)dot){
   if(dot)lv_obj_remove_flag(moveDots[i],LV_OBJ_FLAG_HIDDEN);
   else lv_obj_add_flag(moveDots[i],LV_OBJ_FLAG_HIDDEN);
   renderedDot[i]=(uint8_t)dot;
  }
  if(renderedColor[i]!=bg){
   lv_obj_set_style_bg_color(cells[i],lv_color_hex(bg),0);
   renderedColor[i]=bg;
  }
 }
 if(!vsAI){
  // Compact exposure meter: three pips beneath each player's half.
  // A red pip is one check received, not checks delivered.
  if(variant==VARIANT_SPY){
   for(int side=0;side<2;side++){
    if(renderedSpyChecks[side]==spyChecks[side])continue;
    for(int j=0;j<3;j++)if(spyPips[side][j])
     lv_obj_set_style_bg_color(spyPips[side][j],
        lv_color_hex(j<spyChecks[side]?0xFF5663:0x19482E),0);
    renderedSpyChecks[side]=spyChecks[side];
   }
  }
  const int active=blackToMove?1:0;
  if(renderedTurn!=active){
   for(int side=0;side<2;side++){
    if(!turnFrame[side])continue;
    if(side==active)lv_obj_remove_flag(turnFrame[side],LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(turnFrame[side],LV_OBJ_FLAG_HIDDEN);
   }
   renderedTurn=active;
  }
 }

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
 lv_obj_set_style_radius(b,3,0);
 lv_obj_set_style_bg_color(b,lv_color_hex(0x0B2417),0);
 lv_obj_set_style_border_color(b,lv_color_hex(0x56FF73),0);
 lv_obj_set_style_border_width(b,2,0);
 lv_obj_set_style_shadow_width(b,0,0);
 lv_obj_set_style_bg_color(b,lv_color_hex(0x17552D),LV_STATE_PRESSED);
 lv_obj_set_style_text_color(b,lv_color_hex(0x98FFAA),0);
 lv_obj_add_event_cb(b,actionPressed,LV_EVENT_CLICKED,(void*)(uintptr_t)action);
 lv_obj_t *l=lv_label_create(b);
 lv_label_set_text(l,caption);
 lv_obj_set_style_text_color(l,lv_color_hex(0xA8FFB9),0);
 lv_obj_center(l);
 return b;
}


// All modal changes occur while holding the LVGL adapter lock.
// New Game does not reset the current match until a choice is made.
static void closeModeModal(){
 if(modeOverlay){lv_obj_delete(modeOverlay);modeOverlay=nullptr;}
}
static lv_obj_t *rotatedButton(lv_obj_t *parent,int x,int y,int graphic,int action){
 lv_obj_t *b=lv_button_create(parent);
 lv_obj_set_pos(b,x,y);lv_obj_set_size(b,66,182);
 lv_obj_set_style_radius(b,3,0);
 lv_obj_set_style_bg_color(b,lv_color_hex(0x0B2417),0);
 lv_obj_set_style_border_color(b,lv_color_hex(0x56FF73),0);
 lv_obj_set_style_border_width(b,2,0);
 lv_obj_set_style_bg_color(b,lv_color_hex(0x17552D),LV_STATE_PRESSED);
 lv_obj_set_style_shadow_width(b,0,0);
 lv_obj_add_event_cb(b,actionPressed,LV_EVENT_CLICKED,(void*)(uintptr_t)action);
 lv_obj_t *label=lv_image_create(b);
 lv_image_set_src(label,&chess_rotated_buttons[graphic]);
 lv_obj_set_style_image_recolor_opa(label,LV_OPA_TRANSP,0);
 lv_obj_center(label);
 lv_obj_remove_flag(label,LV_OBJ_FLAG_CLICKABLE);
 return b;
}
static void openSelectionModal(){
 if(modeOverlay)return;
 modeOverlay=lv_obj_create(lv_screen_active());
 lv_obj_set_pos(modeOverlay,0,0);
 lv_obj_set_size(modeOverlay,800,480);
 lv_obj_set_style_radius(modeOverlay,0,0);
 lv_obj_set_style_pad_all(modeOverlay,0,0);
 lv_obj_set_style_border_width(modeOverlay,0,0);
 lv_obj_set_style_bg_color(modeOverlay,lv_color_hex(0x020805),0);
 lv_obj_set_style_bg_opa(modeOverlay,LV_OPA_90,0);
 lv_obj_add_flag(modeOverlay,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_remove_flag(modeOverlay,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_t *panel=lv_obj_create(modeOverlay);
 lv_obj_set_pos(panel,206,16);
 lv_obj_set_size(panel,388,448);
 lv_obj_set_style_radius(panel,5,0);
 lv_obj_set_style_bg_color(panel,lv_color_hex(0x071A10),0);
 lv_obj_set_style_border_color(panel,lv_color_hex(0x56FF73),0);
 lv_obj_set_style_border_width(panel,2,0);
 lv_obj_set_style_shadow_width(panel,0,0);
 lv_obj_set_style_pad_all(panel,0,0);
 lv_obj_remove_flag(panel,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_t *title=lv_label_create(panel);
 lv_label_set_text(title,vsAI?"CHOOSE DIFFICULTY":"CHOOSE GAME MODE");
 lv_obj_set_style_text_color(title,lv_color_hex(0xA8FFB9),0);
 lv_obj_set_style_text_font(title,&lv_font_montserrat_24,0);
 lv_obj_align(title,LV_ALIGN_TOP_MID,0,18);
 if(vsAI){
  newButton(panel,48,65,292,55,"BEGINNER",15);
  newButton(panel,48,128,292,55,"NOVICE",16);
  newButton(panel,48,191,292,55,"INTERMEDIATE",17);
  newButton(panel,48,254,292,55,"MAX ENGINE",18);
 }else{
  newButton(panel,48,66,292,53,"STANDARD",11);
  newButton(panel,48,126,292,53,"KNIGHTS ONLY",12);
  newButton(panel,48,186,292,53,"PAWNS ONLY",13);
  newButton(panel,48,246,292,53,"SPY KINGS",19);
 }
 newButton(panel,76,362,236,52,"CANCEL",14);
}

// An end-of-game result appears once per game, above the board.
// New Game reopens selection; Main Menu goes home.
static void showEndModal(){
 if(!inGame||!gameOver||endModalShown)return;
 endModalShown=true;
 endOverlay=lv_obj_create(lv_screen_active());
 lv_obj_set_pos(endOverlay,0,0);lv_obj_set_size(endOverlay,800,480);
 lv_obj_set_style_radius(endOverlay,0,0);
 lv_obj_set_style_pad_all(endOverlay,0,0);
 lv_obj_set_style_border_width(endOverlay,0,0);
 lv_obj_set_style_bg_color(endOverlay,lv_color_hex(0x020805),0);
 lv_obj_set_style_bg_opa(endOverlay,LV_OPA_90,0);
 lv_obj_remove_flag(endOverlay,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_add_flag(endOverlay,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_t *panel=lv_obj_create(endOverlay);
 lv_obj_set_pos(panel,192,54);lv_obj_set_size(panel,416,372);
 lv_obj_set_style_bg_color(panel,lv_color_hex(0x071A10),0);
 lv_obj_set_style_border_color(panel,lv_color_hex(0x56FF73),0);
 lv_obj_set_style_border_width(panel,2,0);
 lv_obj_set_style_radius(panel,5,0);
 lv_obj_set_style_pad_all(panel,0,0);
 lv_obj_remove_flag(panel,LV_OBJ_FLAG_SCROLLABLE);
 const char *headline=endReason==END_INFILTRATION?"INFILTRATION":
                       endReason==END_EXPOSED?"SPOTTED":
                       endReason==END_CHECKMATE?"CHECKMATE":
                       endReason==END_STALEMATE?"STALEMATE":"DRAW";
 lv_obj_t *title=lv_label_create(panel);
 lv_label_set_text(title,headline);
 lv_obj_set_style_text_font(title,&lv_font_montserrat_28,0);
 lv_obj_set_style_text_color(title,lv_color_hex(
   (endReason==END_EXPOSED||endReason==END_CHECKMATE)?0xFF606A:0x70FF93),0);
 lv_obj_align(title,LV_ALIGN_TOP_MID,0,35);
 lv_obj_t *outcome=lv_label_create(panel);
 lv_obj_set_width(outcome,370);
 lv_label_set_long_mode(outcome,LV_LABEL_LONG_WRAP);
 lv_obj_set_style_text_align(outcome,LV_TEXT_ALIGN_CENTER,0);
 lv_obj_set_style_text_color(outcome,lv_color_hex(0xC8F9D4),0);
 lv_obj_set_style_text_font(outcome,&lv_font_montserrat_24,0);
 const char *who=winnerSide<0?"DRAW":winnerSide==0?"WHITE WINS":"BLACK WINS";
 lv_label_set_text(outcome,who);
 lv_obj_align(outcome,LV_ALIGN_TOP_MID,0,93);
 lv_obj_t *detail=lv_label_create(panel);
 lv_obj_set_width(detail,370);
 lv_label_set_long_mode(detail,LV_LABEL_LONG_WRAP);
 lv_obj_set_style_text_align(detail,LV_TEXT_ALIGN_CENTER,0);
 lv_obj_set_style_text_color(detail,lv_color_hex(0x82B49A),0);
 const char *description=endReason==END_INFILTRATION?"Enemy back rank reached":
    endReason==END_EXPOSED?"Third check: spy exposed":
    endReason==END_CHECKMATE?(variant==VARIANT_SPY&&!vsAI?
       "Spy Kings: checkmate is a draw":"The king has been checkmated"):
    "No legal moves remain";
 lv_label_set_text(detail,description);
 lv_obj_align(detail,LV_ALIGN_TOP_MID,0,151);
 if(variant==VARIANT_SPY&&!vsAI){
  lv_obj_t *counter=lv_label_create(panel);
  char stats[64];
  snprintf(stats,sizeof(stats),"CHECKS RECEIVED: WHITE %u/3  BLACK %u/3",
           (unsigned)spyChecks[0],(unsigned)spyChecks[1]);
  lv_label_set_text(counter,stats);
  lv_obj_set_style_text_color(counter,lv_color_hex(0x9DECAC),0);
  lv_obj_align(counter,LV_ALIGN_TOP_MID,0,196);
 }
 newButton(panel,30,271,168,57,"NEW GAME",1);
 newButton(panel,218,271,168,57,"MAIN MENU",7);
}

static void createGameUI(){
 inGame=true;modeOverlay=nullptr;endOverlay=nullptr;endModalShown=false;
 turnFrame[0]=turnFrame[1]=nullptr;
 for(int side=0;side<2;side++)for(int j=0;j<3;j++)spyPips[side][j]=nullptr;
 renderedSpyChecks[0]=renderedSpyChecks[1]=255;
 renderedTurn=-1;
 lv_obj_clean(lv_screen_active());
 statusLabel=modeLabel=levelLabel=lastLabel=undoButton=nullptr;
 for(int i=0;i<64;i++){renderedPiece[i]=255;renderedColor[i]=0xFFFFFFFF;renderedDot[i]=255;}
 screen=lv_screen_active();
 lv_obj_set_style_bg_color(screen,lv_color_hex(0x050E0B),0);
 lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
 if(!vsAI){
  // Labels are pre-rotated 90 degrees clockwise for White's viewpoint.
  rotatedButton(screen,32,48,0,1);
  rotatedButton(screen,32,250,1,7);
 }else{
  newButton(screen,34,153,268,66,"NEW GAME",1);
  newButton(screen,34,256,268,66,"MAIN MENU",7);
 }

 // Fixed thin neon perimeter. One inert backing object, no animations.
 lv_obj_t *boardFrame=lv_obj_create(screen);
 lv_obj_set_pos(boardFrame,vsAI?342:180,vsAI?18:20);
 lv_obj_set_size(boardFrame,440,440);
 lv_obj_set_style_radius(boardFrame,2,0);
 lv_obj_set_style_pad_all(boardFrame,0,0);
 lv_obj_set_style_bg_opa(boardFrame,LV_OPA_TRANSP,0);
 lv_obj_set_style_border_width(boardFrame,2,0);
 lv_obj_set_style_border_color(boardFrame,lv_color_hex(0x26984B),0);
 lv_obj_set_style_shadow_width(boardFrame,0,0);
 lv_obj_remove_flag(boardFrame,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_remove_flag(boardFrame,LV_OBJ_FLAG_SCROLLABLE);
 // Every mode needs a full set of board objects before drawBoard().
 for(int i=0;i<64;i++){
  int r=i/8,c=i%8;
  // A plain clickable cell avoids LVGL button press recolor styles that
  // propagate into child sprites and lock up software rendering on touch.
  cells[i]=lv_obj_create(screen);
  lv_obj_add_flag(cells[i],LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(cells[i],LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(cells[i],LV_OPA_COVER,0);
  lv_obj_set_style_image_recolor_opa(cells[i],LV_OPA_TRANSP,0);
  // Portrait 2P: device held clockwise, board and touch mapped to
  // physical landscape framebuffer. Viewed portrait board is 432x432,
  // centered horizontally and vertically.
  const int px=vsAI?346+c*54:184+(7-r)*54;
  const int py=vsAI?22+r*54:24+c*54;
  lv_obj_set_pos(cells[i],px,py);
  lv_obj_set_size(cells[i],54,54);
  lv_obj_set_style_radius(cells[i],0,0);
  lv_obj_set_style_border_width(cells[i],0,0);
  lv_obj_set_style_shadow_width(cells[i],0,0);
  lv_obj_set_style_pad_all(cells[i],0,0);
  lv_obj_add_event_cb(cells[i],boardPressed,LV_EVENT_CLICKED,(void*)(uintptr_t)(((r<<4)|c)));
  pieces[i]=lv_image_create(cells[i]);
  // The LVGL style inherited from the button activates image recoloring.
  // Recoloring a whole sprite triggers slow software pixel processing and WDT.
  // Our sprites already contain their final colors; disable recoloring.
  lv_obj_set_style_image_recolor_opa(pieces[i],LV_OPA_TRANSP,LV_PART_MAIN);
  lv_obj_set_style_image_opa(pieces[i],LV_OPA_COVER,LV_PART_MAIN);
  lv_obj_set_size(pieces[i],44,44);
  lv_obj_center(pieces[i]);
  lv_obj_remove_flag(pieces[i],LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(pieces[i],LV_OBJ_FLAG_HIDDEN);
  moveDots[i]=lv_obj_create(cells[i]);
  lv_obj_set_size(moveDots[i],16,16);
  lv_obj_set_style_radius(moveDots[i],LV_RADIUS_CIRCLE,0);
  lv_obj_set_style_bg_color(moveDots[i],lv_color_hex(0x27FB64),0);
  lv_obj_set_style_bg_opa(moveDots[i],LV_OPA_COVER,0);
  lv_obj_set_style_border_width(moveDots[i],0,0);
  lv_obj_set_style_shadow_width(moveDots[i],0,0);
  lv_obj_set_style_pad_all(moveDots[i],0,0);
  lv_obj_center(moveDots[i]);
  lv_obj_remove_flag(moveDots[i],LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(moveDots[i],LV_OBJ_FLAG_HIDDEN);
 }
 if(!vsAI){
  // Portrait board is 432x432 at (184,24). A half is 216x432.
  // Borders are non-clickable objects layered above the squares.
  for(int side=0;side<2;side++){
   turnFrame[side]=lv_obj_create(screen);
   lv_obj_set_pos(turnFrame[side],184+side*216,24);
   lv_obj_set_size(turnFrame[side],216,432);
   lv_obj_set_style_radius(turnFrame[side],0,0);
   lv_obj_set_style_pad_all(turnFrame[side],0,0);
   lv_obj_set_style_bg_opa(turnFrame[side],LV_OPA_TRANSP,0);
   lv_obj_set_style_border_width(turnFrame[side],4,0);
   // U-shaped border: never draw the shared center seam.
   lv_obj_set_style_border_side(turnFrame[side],(lv_border_side_t)(side==0?
       (LV_BORDER_SIDE_LEFT|LV_BORDER_SIDE_TOP|LV_BORDER_SIDE_BOTTOM):
       (LV_BORDER_SIDE_RIGHT|LV_BORDER_SIDE_TOP|LV_BORDER_SIDE_BOTTOM)),0);
   lv_obj_set_style_border_color(turnFrame[side],lv_color_hex(0x56FF73),0);
   lv_obj_set_style_shadow_width(turnFrame[side],0,0);
   lv_obj_remove_flag(turnFrame[side],LV_OBJ_FLAG_CLICKABLE);
   lv_obj_remove_flag(turnFrame[side],LV_OBJ_FLAG_SCROLLABLE);
   lv_obj_add_flag(turnFrame[side],LV_OBJ_FLAG_HIDDEN);
  }
 }
 if(!vsAI&&variant==VARIANT_SPY){
  for(int side=0;side<2;side++)for(int j=0;j<3;j++){
   lv_obj_t *pip=lv_obj_create(screen);
   spyPips[side][j]=pip;
   lv_obj_set_pos(pip,(side==0?252:468)+j*28,464);
   lv_obj_set_size(pip,18,8);
   lv_obj_set_style_radius(pip,2,0);
   lv_obj_set_style_pad_all(pip,0,0);
   lv_obj_set_style_border_width(pip,1,0);
   lv_obj_set_style_border_color(pip,lv_color_hex(0x316444),0);
   lv_obj_set_style_bg_color(pip,lv_color_hex(0x19482E),0);
   lv_obj_set_style_shadow_width(pip,0,0);
   lv_obj_remove_flag(pip,LV_OBJ_FLAG_CLICKABLE);
   lv_obj_remove_flag(pip,LV_OBJ_FLAG_SCROLLABLE);
  }
 }
 drawBoard();
}

static void createHome(){
 inGame=false;modeOverlay=nullptr;endOverlay=nullptr;endModalShown=false;
 turnFrame[0]=turnFrame[1]=nullptr;renderedTurn=-1;
 lv_obj_clean(lv_screen_active());
 screen=lv_screen_active();
 statusLabel=modeLabel=levelLabel=lastLabel=undoButton=nullptr;
 lv_obj_set_style_bg_color(screen,lv_color_hex(0x050E0B),0);
 lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_t *brand=lv_image_create(screen);
 lv_image_set_src(brand,&chess_logo);
 lv_obj_set_style_image_recolor_opa(brand,LV_OPA_TRANSP,0);
 lv_obj_set_pos(brand,10,20);
 lv_obj_remove_flag(brand,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_t *divider=lv_obj_create(screen);
 lv_obj_set_pos(divider,415,60);lv_obj_set_size(divider,2,360);
 lv_obj_set_style_bg_color(divider,lv_color_hex(0x164A29),0);
 lv_obj_set_style_border_width(divider,0,0);
 lv_obj_set_style_radius(divider,0,0);
 lv_obj_set_style_pad_all(divider,0,0);
 lv_obj_remove_flag(divider,LV_OBJ_FLAG_CLICKABLE);
 newButton(screen,466,160,276,72,"1 PLAYER",5);
 newButton(screen,466,275,276,72,"2 PLAYERS",6);
}
static void resetGame(){
 if(!vsAI&&variant==VARIANT_KNIGHTS){
  // Kings and 15 knights per side. No castling.
  mcumax_set_fen_position("nnnnknnn/nnnnnnnn/8/8/8/8/NNNNNNNN/NNNNKNNN w - - 0 1");
 }else if(!vsAI&&variant==VARIANT_PAWNS){
  // Kings and 15 pawns per side. Rear-rank pawns advance after
  // the normal pawns clear the way. No castling.
  mcumax_set_fen_position("ppppkppp/pppppppp/8/8/8/8/PPPPPPPP/PPPPKPPP w - - 0 1");
 }else mcumax_init();
 historyCount=0;selected=-1;awaitingAI=false;
 endReason=END_NONE;winnerSide=-1;spyChecks[0]=spyChecks[1]=0;endModalShown=false;
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
   moveText(m,false);selected=-1;resolveSpyMove(m);refreshLegal();
   awaitingAI=vsAI&&!gameOver&&mcumax_get_current_side()==0x10;
   return;
  }
 }
 selected=own&&hasFrom(sq)?sq:-1;
 Serial.printf("Chess: selection=%d\\n",selected);
}
void setup(){
 Serial.begin(115200);
 randomSeed(esp_random());
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
 createHome();
 esp_lv_adapter_unlock();
 Serial.println("Chess Station ready");
}
void loop(){
 const int a=queuedAction;
 const int sq=queuedSquare;
 if(a)queuedAction=0;
 if(sq>=0)queuedSquare=-1;
 bool dirty=false;
 if(a){
  // Changing modes/difficulty only happens once the user actually picks.
  if(a==5||a==6||a==1){
   if(a!=1)vsAI=(a==5);
   if(!inGame||a==1){
    if(esp_lv_adapter_lock(1000)==ESP_OK){
     openSelectionModal();
     esp_lv_adapter_unlock();
    }
   }
  }else if(a==7){
   if(esp_lv_adapter_lock(1000)==ESP_OK){
    createHome();
    esp_lv_adapter_unlock();
   }
  }else if(a==14){
   if(esp_lv_adapter_lock(1000)==ESP_OK){
    closeModeModal();
    esp_lv_adapter_unlock();
   }
  }else if(modeOverlay&&((vsAI&&a>=15&&a<=18)||(!vsAI&&((a>=11&&a<=13)||a==19)))){
   if(vsAI)level=(uint8_t)(a-14);
   else variant=(a==19)?VARIANT_SPY:(GameVariant)(a-11);
   resetGame();
   if(esp_lv_adapter_lock(1000)==ESP_OK){
    createGameUI();
    esp_lv_adapter_unlock();
   }else{
    Serial.println("Chess: LVGL lock timeout on new game");
   }
  }
 }
 // Prevent taps behind a modal from executing queued chess moves.
 if(inGame&&!modeOverlay&&sq>=0&&sq<128&&!(sq&0x88)){
  squareClicked((uint8_t)sq);
  dirty=true;
 }
 if(dirty&&inGame&&esp_lv_adapter_lock(1000)==ESP_OK){
  drawBoard();
  showEndModal();
  esp_lv_adapter_unlock();
 }
 if(inGame&&!modeOverlay&&awaitingAI){
  delay(60);
  mcumax_move best=selectComputerMove();
  if(best.from!=MCUMAX_SQUARE_INVALID&&mcumax_play_move(best)){
   if(historyCount<512)history[historyCount++]=best;
   moveText(best,true);
   resolveSpyMove(best);
  }else strcpy(lastText,"Computer could not find a move");
  awaitingAI=false;selected=-1;refreshLegal();
  if(esp_lv_adapter_lock(-1)==ESP_OK){drawBoard();showEndModal();esp_lv_adapter_unlock();}
 }
 delay(12);
}
