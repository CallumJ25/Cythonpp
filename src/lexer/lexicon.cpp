#include <fstream>
#include <string>

#include "lexicon.h"

void ReadFile(std::string filepath) {

std::ifstream file(filepath);

std::string line;

if (!file) {
  std::cout << "Read Error" << std::endl; 
}

while (std::getline(file, line)) {
  // Output the text from the file
  std::cout << line << std::endl;
};
}

struct token {
  std::string id;
  std::string lexeme;
  int line_number;
  int column_number;
};
