"""Apply explicit 1x/2x/4x gameplay timing without changing game physics."""
from pathlib import Path

path = Path("sketch/ShapeSiege/ShapeSiege.ino")
source = path.read_text()

def replace_one(old, new):
    global source
    count = source.count(old)
    if count != 1:
        raise RuntimeError(f"Expected exactly one patch anchor, got {count}: {old[:100]!r}")
    source = source.replace(old, new, 1)

replace_one(
    "lv_obj_t *screen,*field,*hudWave,*hudCash,*hudLives,*hint,*mapButton,*actionButton,*towerButtons[4],*resultLabel;",
    "lv_obj_t *screen,*field,*hudWave,*hudCash,*hudLives,*hint,*mapButton,*actionButton,*speedButton,*towerButtons[4],*resultLabel;"
)
replace_one(
    "bool running=false,ended=false;\n",
    "bool running=false,ended=false;\n"
    "constexpr uint8_t speeds[3]={1,2,4};\n"
    "uint8_t speedIndex=2; // Default 4x, cycle to 1x and 2x via the top-right button.\n"
)
replace_one(
    "void startClick(lv_event_t*){start();}",
    "void startClick(lv_event_t*){start();}\n"
    "void speedClick(lv_event_t*){\n"
    " speedIndex=(speedIndex+1)%3;\n"
    " char b[20];snprintf(b,sizeof b,\"SPEED %dX\",speeds[speedIndex]);\n"
    " lv_label_set_text(speedButton,b);\n"
    "}"
)
replace_one(
    ' makeButton(actionButton,649,430,139,42,"START WAVE",startClick);\n',
    ' makeButton(actionButton,649,430,139,42,"START WAVE",startClick);\n'
    ' makeButton(speedButton,674,5,114,38,"SPEED 4X",speedClick);\n'
)
replace_one(
    " actionButton=lv_obj_get_child(actionButton,0);\n",
    " actionButton=lv_obj_get_child(actionButton,0);\n"
    " speedButton=lv_obj_get_child(speedButton,0);\n"
)
replace_one(
    "     Game::tick(dt);Game::lastMs=now;",
    "     // Four normal-length simulation steps preserves spawn/cooldown behavior.\n"
    "     for(uint8_t step=0;step<Game::speeds[Game::speedIndex];step++)Game::tick(dt);\n"
    "     Game::lastMs=now;"
)
path.write_text(source)
print("Patched Shape Siege: default 4x; selectable 1x / 2x / 4x; preserved simulation step length.")
