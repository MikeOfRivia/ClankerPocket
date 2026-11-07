#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <esp_err.h>
#include <lvgl.h>
#include "esp_lv_adapter_arduino.h"
#include "mcu-max.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

static lv_obj_t *cells[64], *pieces[64], *moveDots[64], *statusLabel, *modeLabel, *levelLabel, *lastLabel, *undoButton;
static bool inGame=false, twoPlayerSetup=false;
enum GameVariant : uint8_t { VARIANT_STANDARD=0, VARIANT_KNIGHTS=1, VARIANT_PAWNS=2 };
static GameVariant variant=VARIANT_STANDARD;
static lv_obj_t *variantLabel=nullptr, *modeOverlay=nullptr;
static lv_obj_t *turnFrame[2]={nullptr,nullptr};
static int8_t renderedTurn=-1;
static const char *variantName(){return variant==VARIANT_KNIGHTS?"KNIGHTS ONLY":variant==VARIANT_PAWNS?"PAWNS ONLY":"STANDARD";}
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

static void refreshLegal(){
 validCount=mcumax_search_valid_moves(validMoves,256);
 gameOver=(validCount==0);
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
  uint32_t bg=dark?0x71926F:0xE9E3C9;
  // Subtle last-move markers on BOTH origin and destination, including the
  // computer's reply. Derived from history so Undo and New Game stay correct.
  if(historyCount>0){
   const mcumax_move last=history[historyCount-1];
   if(sq==last.from||sq==last.to)bg=dark?0x8C9965:0xD1CA82;
  }
  // Capture squares are red, but the selected piece remains gold.
  const bool legalTarget=selected>=0&&canMove((uint8_t)selected,sq);
  const bool capture=legalTarget&&((p&7)!=MCUMAX_EMPTY);
  if(selected==(int)sq)bg=0xE6B448;
  else if(capture)bg=0xBE4950;
  // Check takes visual priority over selection and move markers.
  if(sq==checkedKing)bg=0xDC343F;
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
  // Three-sided yellow turn indicator: outside edge and top/bottom only.
  // The center seam stays clear. White=left, Black=right in framebuffer.
  const int active=blackToMove?1:0;
  if(renderedTurn!=active){
   for(int side=0;side<2;side++){
    if(!turnFrame[side])continue;
    if(side==active)lv_obj_remove_flag(turnFrame[side],LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(turnFrame[side],LV_OBJ_FLAG_HIDDEN);
   }
   renderedTurn=active;
  }
  return;
 }
 lv_label_set_text(statusLabel,gameOver?(checkedKing>=0?"CHECKMATE!":"STALEMATE"):(checkedKing>=0?(blackToMove?"CHECK! BLACK KING":"CHECK! WHITE KING"):(awaitingAI?"Computer thinking...":blackToMove?"Black to move":"White to move")));
 lv_obj_set_style_text_color(statusLabel,checkedKing>=0?lv_color_hex(0xFF626B):lv_color_hex(0xC3D6EC),0);

 char lvl[48];snprintf(lvl,sizeof(lvl),"LEVEL %d/4: %s",level,levelNames[level-1]);
 lv_label_set_text(levelLabel,lvl);
 lv_label_set_text(lastLabel,lastText);
 if(undoButton){
  if(level<=2)lv_obj_remove_flag(undoButton,LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(undoButton,LV_OBJ_FLAG_HIDDEN);
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
 lv_obj_set_style_radius(b,8,0);
 lv_obj_set_style_bg_color(b,lv_color_hex(0x34445A),0);
 lv_obj_add_event_cb(b,actionPressed,LV_EVENT_CLICKED,(void*)(uintptr_t)action);
 lv_obj_t *l=lv_label_create(b);lv_label_set_text(l,caption);lv_obj_center(l);
 return b;
}


// LVGL objects must be created/destroyed while holding the adapter lock.
// This is a non-destructive overlay: the underlying game and board persist.
static void closeModeModal(){
 if(modeOverlay){lv_obj_delete(modeOverlay);modeOverlay=nullptr;}
}
static void openModeModal(){
 if(modeOverlay||vsAI)return;
 modeOverlay=lv_obj_create(screen);
 lv_obj_set_pos(modeOverlay,0,0);
 lv_obj_set_size(modeOverlay,800,480);
 lv_obj_set_style_radius(modeOverlay,0,0);
 lv_obj_set_style_pad_all(modeOverlay,0,0);
 lv_obj_set_style_border_width(modeOverlay,0,0);
 lv_obj_set_style_bg_color(modeOverlay,lv_color_hex(0x0A111B),0);
 lv_obj_set_style_bg_opa(modeOverlay,LV_OPA_90,0);
 lv_obj_add_flag(modeOverlay,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_remove_flag(modeOverlay,LV_OBJ_FLAG_SCROLLABLE);

 lv_obj_t *panel=lv_obj_create(modeOverlay);
 lv_obj_set_pos(panel,212,20);
 lv_obj_set_size(panel,376,440);
 lv_obj_set_style_radius(panel,16,0);
 lv_obj_set_style_bg_color(panel,lv_color_hex(0x182637),0);
 lv_obj_set_style_border_color(panel,lv_color_hex(0xCDA45C),0);
 lv_obj_set_style_border_width(panel,2,0);
 lv_obj_set_style_pad_all(panel,0,0);
 lv_obj_remove_flag(panel,LV_OBJ_FLAG_SCROLLABLE);

 lv_obj_t *heading=lv_label_create(panel);
 lv_label_set_text(heading,"CHOOSE GAME MODE");
 lv_obj_set_style_text_color(heading,lv_color_hex(0xF4D58C),0);
 lv_obj_set_style_text_font(heading,&lv_font_montserrat_24,0);
 lv_obj_align(heading,LV_ALIGN_TOP_MID,0,20);

 newButton(panel,48,74,280,56,"STANDARD",11);
 newButton(panel,48,142,280,56,"KNIGHTS ONLY",12);
 newButton(panel,48,210,280,56,"PAWNS ONLY",13);
 newButton(panel,48,348,280,52,"CANCEL",14);
 lv_obj_t *note=lv_label_create(panel);
 lv_label_set_text(note,inGame?"Choosing a mode starts a new game":"Choose a mode, then start the game");
 lv_obj_set_style_text_color(note,lv_color_hex(0xB3C3D4),0);
 lv_obj_align(note,LV_ALIGN_TOP_MID,0,293);
}

static void createGameUI(){
 inGame=true;twoPlayerSetup=false;modeOverlay=nullptr;
 turnFrame[0]=turnFrame[1]=nullptr;
 renderedTurn=-1;
 lv_obj_clean(lv_screen_active());
 statusLabel=modeLabel=levelLabel=lastLabel=undoButton=nullptr;
 for(int i=0;i<64;i++){renderedPiece[i]=255;renderedColor[i]=0xFFFFFFFF;renderedDot[i]=255;}
 screen=lv_screen_active();
 lv_obj_set_style_bg_color(screen,lv_color_hex(0x111C2A),0);
 lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
 if(!vsAI){
  // In portrait 2P these controls sit outside the centered board.
  newButton(screen,8,8,120,42,"MENU",7);
  newButton(screen,8,60,120,42,"MODE",10);
 }

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
  const int px=vsAI?22+c*54:184+(7-r)*54;
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
  lv_obj_set_style_bg_color(moveDots[i],lv_color_hex(0x2874BA),0);
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
   lv_obj_set_style_border_side(turnFrame[side],side==0?
       (LV_BORDER_SIDE_LEFT|LV_BORDER_SIDE_TOP|LV_BORDER_SIDE_BOTTOM):
       (LV_BORDER_SIDE_RIGHT|LV_BORDER_SIDE_TOP|LV_BORDER_SIDE_BOTTOM),0);
   lv_obj_set_style_border_color(turnFrame[side],lv_color_hex(0xFFD348),0);
   lv_obj_set_style_shadow_width(turnFrame[side],0,0);
   lv_obj_remove_flag(turnFrame[side],LV_OBJ_FLAG_CLICKABLE);
   lv_obj_remove_flag(turnFrame[side],LV_OBJ_FLAG_SCROLLABLE);
   lv_obj_add_flag(turnFrame[side],LV_OBJ_FLAG_HIDDEN);
  }
 }
 if(vsAI){
 statusLabel=lv_label_create(screen);
 lv_obj_set_style_text_color(statusLabel,lv_color_hex(0xC3D6EC),0);
 lv_obj_set_pos(statusLabel,476,45);
 levelLabel=lv_label_create(screen);lv_obj_set_pos(levelLabel,476,105);
 lv_obj_set_style_text_color(levelLabel,lv_color_hex(0xFFFFFF),0);
 newButton(screen,476,180,145,46,"NEW GAME",1);
 undoButton=newButton(screen,633,180,145,46,"UNDO",2);
 newButton(screen,476,238,302,46,"RETURN TO MENU",7);
 newButton(screen,476,296,302,46,"CHANGE DIFFICULTY",4);
 lastLabel=lv_label_create(screen);
 lv_obj_set_width(lastLabel,302);
 lv_label_set_long_mode(lastLabel,LV_LABEL_LONG_WRAP);
 lv_obj_set_style_text_color(lastLabel,lv_color_hex(0xF4D58C),0);
 lv_obj_set_pos(lastLabel,476,364);
 }
 drawBoard();
}

static void createHome(){
 inGame=false;twoPlayerSetup=false;modeOverlay=nullptr;
 turnFrame[0]=turnFrame[1]=nullptr;renderedTurn=-1;
 lv_obj_clean(lv_screen_active());
 screen=lv_screen_active();
 statusLabel=modeLabel=levelLabel=lastLabel=undoButton=variantLabel=nullptr;
 lv_obj_set_style_bg_color(screen,lv_color_hex(0x111C2A),0);
 lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_t *brand=lv_image_create(screen);
 lv_image_set_src(brand,&chess_logo);
 lv_obj_set_style_image_recolor_opa(brand,LV_OPA_TRANSP,0);
 lv_obj_align(brand,LV_ALIGN_TOP_MID,0,0);
 lv_obj_remove_flag(brand,LV_OBJ_FLAG_CLICKABLE);
 newButton(screen,275,260,250,65,"1 PLAYER",5);
 newButton(screen,275,345,250,65,"2 PLAYERS",6);
}
static void createTwoPlayerSetup(){
 inGame=false;twoPlayerSetup=true;modeOverlay=nullptr;
 turnFrame[0]=turnFrame[1]=nullptr;renderedTurn=-1;
 lv_obj_clean(lv_screen_active());
 screen=lv_screen_active();
 statusLabel=modeLabel=levelLabel=lastLabel=undoButton=nullptr;
 lv_obj_set_style_bg_color(screen,lv_color_hex(0x111C2A),0);
 lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_t *heading=lv_label_create(screen);
 lv_label_set_text(heading,"2 PLAYERS");
 lv_obj_set_style_text_font(heading,&lv_font_montserrat_28,0);
 lv_obj_set_style_text_color(heading,lv_color_hex(0xF4D58C),0);
 lv_obj_align(heading,LV_ALIGN_TOP_MID,0,40);
 variantLabel=lv_label_create(screen);
 lv_obj_set_style_text_color(variantLabel,lv_color_hex(0xFFFFFF),0);
 lv_obj_align(variantLabel,LV_ALIGN_TOP_MID,0,120);
 lv_label_set_text(variantLabel,variantName());
 newButton(screen,270,165,260,55,"CHANGE MODE",8);
 newButton(screen,270,239,260,60,"START GAME",9);
 newButton(screen,270,317,260,52,"RETURN TO MENU",7);
 lv_obj_t *tip=lv_label_create(screen);
 lv_label_set_text(tip,"Turn device clockwise for portrait play");
 lv_obj_set_style_text_color(tip,lv_color_hex(0x8EA0B6),0);
 lv_obj_align(tip,LV_ALIGN_BOTTOM_MID,0,-38);
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
 int a=queuedAction;
 int s=queuedSquare;
 if(a)queuedAction=0;
 if(s>=0)queuedSquare=-1;
 bool dirty=false;
 if(a){
  bool rebuild=false;
  int target=0; // 1 main menu, 2 two-player setup, 3 game
  switch(a){
   case 1:if(inGame)resetGame();break;
   case 2:if(inGame&&vsAI&&level<=2)undoMove();break;
   case 4:if(inGame&&vsAI)level=level%4+1;break;
   case 5:vsAI=true;resetGame();rebuild=true;target=3;break;
   case 6:vsAI=false;rebuild=true;target=2;break;
   case 7:rebuild=true;target=1;break;
   case 8:if(twoPlayerSetup&&!vsAI){
    if(esp_lv_adapter_lock(1000)==ESP_OK){openModeModal();esp_lv_adapter_unlock();}
   }break;
   case 9:if(twoPlayerSetup){vsAI=false;resetGame();rebuild=true;target=3;}break;
   case 10:if(inGame&&!vsAI){
    if(esp_lv_adapter_lock(1000)==ESP_OK){openModeModal();esp_lv_adapter_unlock();}
   }break;
   case 11:case 12:case 13:
    if(!vsAI&&modeOverlay){
     variant=(GameVariant)(a-11);
     if(inGame)resetGame();
     if(esp_lv_adapter_lock(1000)==ESP_OK){
      closeModeModal();
      if(inGame)drawBoard();
      else if(twoPlayerSetup&&variantLabel)lv_label_set_text(variantLabel,variantName());
      esp_lv_adapter_unlock();
     }
    }
    break;
   case 14:
    if(esp_lv_adapter_lock(1000)==ESP_OK){closeModeModal();esp_lv_adapter_unlock();}
    break;
   default:break;
  }
  dirty=true;
  if(rebuild){
   if(esp_lv_adapter_lock(1000)==ESP_OK){
    if(target==1)createHome();
    else if(target==2)createTwoPlayerSetup();
    else if(target==3)createGameUI();
    esp_lv_adapter_unlock();
    dirty=false;
   }
  }
 }
 if(inGame&&s>=0&&s<128&&!(s&0x88)){
  squareClicked((uint8_t)s);dirty=true;
 }
 if(dirty){
  if(esp_lv_adapter_lock(1000)==ESP_OK){
   if(inGame)drawBoard();
   else if(twoPlayerSetup&&variantLabel)
    lv_label_set_text(variantLabel,variantName());
   esp_lv_adapter_unlock();
  }else Serial.println("Chess: LVGL lock timeout");
 }
 if(inGame&&awaitingAI){
  delay(60);
  mcumax_move best=selectComputerMove();
  if(best.from!=MCUMAX_SQUARE_INVALID&&mcumax_play_move(best)){
   if(historyCount<512)history[historyCount++]=best;
   moveText(best,true);
  }else strcpy(lastText,"Computer could not find a move");
  awaitingAI=false;selected=-1;refreshLegal();
  if(esp_lv_adapter_lock(-1)==ESP_OK){drawBoard();esp_lv_adapter_unlock();}
 }
 delay(12);
}
