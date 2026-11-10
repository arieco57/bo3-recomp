// Blocking dialogs for setup flows outside the game's own UI (online account): a choice between
// buttons (SDL message box) and a form with text fields (zenity on the desktop, a Java dialog on
// Android - SDL has no text-input dialog).
#pragma once
#include <string>
#include <vector>

struct UiField { std::string label; bool secret = false; std::string value; };

int UiChoice(const std::string& title, const std::string& text, const std::vector<std::string>& buttons);   // index, -1 = closed
bool UiForm(const std::string& title, const std::string& text, std::vector<UiField>& fields);              // false = cancelled
void UiMessage(const std::string& title, const std::string& text);
