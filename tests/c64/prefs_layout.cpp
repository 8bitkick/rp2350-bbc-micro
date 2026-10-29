#include "../../deps/frank-c64/src/Prefs.h"
#include <cassert>
#include <cstdio>
int main(){
 Prefs p;assert(p.NormalCycles==63);assert(p.ROMSetDefs.empty());assert(p.ButtonMapDefs.empty());
 p.DrivePath[0]="presto.d64";p.CartridgePath="test";p.ROMSetDefs["test"].BasicROMPath="basic";
 Prefs q=p;assert(q.DrivePath[0]=="presto.d64");assert(q.CartridgePath=="test");assert(q.ROMSetDefs["test"].BasicROMPath=="basic");
 puts("preferences construction and copy: passed");
}
