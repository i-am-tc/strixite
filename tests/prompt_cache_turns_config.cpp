#include "serve/server_config.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace strix;
void check(bool v) { if (!v) throw std::runtime_error("config assertion failed"); }
int main(int argc, char **argv) {
 try {
  if (argc != 2) return 2;
  auto make=[] {return ServerSettings(strix_server_options("/models"));};
  auto s=make();check(s.integer("prompt-cache-turns")==4);check(s.integer("prompt-cache-checkpoints")==2);
  for (auto n : {1,64}) {auto c=make();c.load({"--prompt-cache-turns",std::to_string(n)});check(c.integer("prompt-cache-turns")==n);}
  for (auto n : {"0","65","x","1.5"}) {bool rejected=false;try {auto c=make();c.load({"--prompt-cache-turns",n});}catch(const std::exception&){rejected=true;}check(rejected);}
  auto path=std::filesystem::absolute(argv[1]);{std::ofstream f(path);f << "prompt-cache-turns = 6\nprompt-cache-checkpoints = 2\n";}
  auto c=make();c.load({"--config",path.string(),"--prompt-cache-turns","4"});check(c.integer("prompt-cache-turns")==4);check(c.setting("prompt-cache-turns").source=="flag");
  {std::ofstream f(path);f << c.render();}auto d=make();d.load_file(path.string());check(d.integer("prompt-cache-turns")==4);check(d.integer("prompt-cache-checkpoints")==2);
  check(d.usage("strix_server").find("--prompt-cache-turns")!=std::string::npos);
  std::cout << "PASS: default, range, CLI, file, precedence, render roundtrip and help; checkpoints independent\n";
 }catch(const std::exception&e){std::cerr << e.what() << '\n';return 1;}
}
