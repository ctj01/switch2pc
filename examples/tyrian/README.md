# OpenTyrian

An unmodified SDL2 game. Sources are not vendored:

    git clone https://github.com/opentyrian/opentyrian third_party/opentyrian

Data: the freeware Tyrian 2.1 release (tyrian21.zip from camanis.net),
extracted so that `examples/tyrian/romfs/data/tyrian.shp` exists. Then, from
Git Bash with devkitPro installed:

    scripts/build_sdl2.sh          # once
    scripts/build_opentyrian.sh    # examples/tyrian/build/tyrian.nso
    scripts/run_game.sh tyrian 2
