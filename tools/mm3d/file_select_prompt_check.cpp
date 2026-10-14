// Reuse native font/atlas fixtures and existing prompt regression coverage.
#define main native_inventory_checks_main
#include "native_inventory_check.cpp"
#undef main
int main() {
    NotebookPromptChecks();
    std::cout << "File select and shared native prompt checks passed\n";
}
