// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Latedozer. Free software under the GNU GPL version 3; see LICENSE.
// How saved affix ids map to MagicPrefix / MagicSuffix rows, on COPIES of the user's saves (Save Scan's reader).
#include "../../Save Scan/save_core.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <strings.h>
#include <cstring>
using namespace savescan;
static std::string Slurp(const std::filesystem::path& p){std::ifstream in(p,std::ios::binary);std::stringstream s;s<<in.rdbuf();return s.str();}
typedef std::vector<std::string> Row;
struct T { Row head; std::vector<Row> rows; int col(const char* n) const { for(size_t i=0;i<head.size();++i) if(strcasecmp(head[i].c_str(),n)==0) return (int)i; return -1; } };
static T Read(const std::string& text, bool keepExp){ T t; std::istringstream in(text); std::string l; bool first=true; while(std::getline(in,l)){ if(!l.empty()&&l.back()=='\r') l.pop_back(); Row r; std::stringstream cs(l); std::string c; while(std::getline(cs,c,'\t')) r.push_back(c); if(l.find('\t')!=std::string::npos && l.back()=='\t') r.push_back(""); if(first){t.head=r;first=false;continue;} if(l.empty()) continue; if(!keepExp && !r.empty() && r[0]=="Expansion") continue; t.rows.push_back(r);} return t; }
static int g_failed = 0;
static void Check(bool ok, const std::string& what) { printf("%s%s\n", ok ? "ok    " : "FAIL  ", what.c_str()); g_failed += ok ? 0 : 1; }

// affix_ids_test <saves copy> <stat/item tables> <MagicPrefix.txt folder> <magicsuffix.txt folder>
// A saved prefix / suffix id = the row of its own file, counting every data line (the "Expansion"
// line too); the game's memory drops the Expansion line, so memory row = id - 1 after it.
int main(int argc, char** argv){
  if (argc < 5) { printf("usage: affix_ids_test <saves> <excel> <prefix folder> <suffix folder>\n"); return 2; }
  std::filesystem::path ex=argv[2], van=argv[3], mod=argv[4];
  Tables t; ShapesFromText(t,Slurp(ex/"itemstatcost.txt")); ReadItemTable(t,Slurp(ex/"armor.txt"),kArmor); ReadItemTable(t,Slurp(ex/"weapons.txt"),kWeapon); ReadItemTable(t,Slurp(ex/"misc.txt"),kMisc);
  for (int i=0;i<3;++i){ StatShape s=t.stats.back(); t.stats.push_back(s);} // Live Tables' rows 389-391 (made stats; not in the text)
  T isc=Read(Slurp(ex/"itemstatcost.txt"),false); std::map<std::string,int> statNo; for(size_t i=0;i<isc.rows.size();++i) statNo[isc.rows[i][0]]=(int)i;
  T props=Read(Slurp(ex/"properties.txt"),false); std::map<std::string,std::vector<int>> propStats; int sc[7]; for(int k=0;k<7;++k){ char b[8]; snprintf(b,sizeof b,"stat%d",k+1); sc[k]=props.col(b);}
  for(auto& r: props.rows){ for(int k=0;k<7;++k) if(sc[k]>=0 && sc[k]<(int)r.size() && !r[sc[k]].empty() && statNo.count(r[sc[k]])) propStats[r[0]].push_back(statNo[r[sc[k]]]); }
  T pre=Read(Slurp(van/"MagicPrefix.txt"),true), suf=Read(Slurp(mod/"magicsuffix.txt"),true);
  long ids=0, outside=0, tried=0, matched=0, combinedMatched=0, combinedTried=0;
  for(auto& e: std::filesystem::directory_iterator(argv[1])){
    auto b=Slurp(e.path()); auto r = e.path().extension()==".d2i"? ReadStash((const uint8_t*)b.data(),b.size(),t):ReadCharacter((const uint8_t*)b.data(),b.size(),t);
    for(auto& it: r.items){ std::set<int> have; for(auto& s: it.stats) have.insert(s.stat);
      for(auto& a: it.affixes){ if(a.id==0) continue; ++ids;
        auto check=[&](const T& tab, long row, long& tr, long& ma){ if(row<0||row>=(long)tab.rows.size()) return false;
          for(int m=1;m<=3;++m){ char k[16]; snprintf(k,sizeof k,"mod%dcode",m); int c=tab.col(k); if(c<0||c>=(int)tab.rows[row].size()) continue; auto p=propStats.find(tab.rows[row][c]); if(p==propStats.end()) continue;
            ++tr; bool ok=false; for(int s: p->second) ok=ok||have.count(s); ma+=ok; } return true; };
        if(!check(a.suffix?suf:pre, a.id, tried, matched)) ++outside;
        const long c = (long)a.id - (long)suf.rows.size(); // the rule it is not: one vector, suffixes first (a prefix id after them)
        if (!a.suffix && c >= 0) check(pre, c, combinedTried, combinedMatched);
      } } }
  printf("note  %ld saved affix ids; prefix table %zu rows, suffix %zu (Expansion counted)\n", ids, pre.rows.size(), suf.rows.size());
  Check(ids > 0 && outside == 0, "every saved id is a row of its own table (" + std::to_string(outside) + " past the end)");
  Check(tried > 0 && matched == tried, "every mod of that row is on the item: " + std::to_string(matched) + " of " + std::to_string(tried));
  Check(combinedTried == 0 || combinedMatched * 10 < combinedTried * 8, "one combined vector (prefix ids after the suffixes) would not fit (" + std::to_string(combinedMatched) + " of " + std::to_string(combinedTried) + ")");
  printf(g_failed == 0 ? "affix ids: all passed\n" : "affix ids: %d FAILED\n", g_failed);
  return g_failed == 0 ? 0 : 1;
}
