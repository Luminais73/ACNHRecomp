// Part of lib<game>_game.so (Android): the game's recompiled code as its own library, next to the app's
// lib<game>.so, which holds no game code (CMakeLists.txt, docs/ANDROID.md). The app loads it from the game folder
// and finds the modules' tables (nx_module_<name>) and these two functions in it.
#include <nx/cpu.h>

// The headers this library was built from (CMakeLists.txt: a hash of nx/cpu.h, module.h, runtime.h): the app
// refuses a library built from other ones.
extern "C" __attribute__((visibility("default"))) const char* nx_game_abi() { return NX_GAME_ABI; }

// A guest longjmp: thrown here so that the exception's type is this library's, the one its setjmp frames catch.
extern "C" __attribute__((visibility("default"))) [[noreturn]] void nx_game_longjmp(uint64_t buf, uint64_t val) {
    throw nx::LongJmp{buf, val};
}
