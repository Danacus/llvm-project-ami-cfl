#ifndef LLVM_TRANSFORMS_SECRETS_H
#define LLVM_TRANSFORMS_SECRETS_H

#include <string>

struct SecretVar {
  std::string Func;
  std::string BB;
  std::string Name;
};

#endif // LLVM_TRANSFORMS_SECRETS_H
