// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/linux_upgrade_commands.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "test_util.h"

namespace {

std::string readFile(const char* path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// The "Update method" cell of the docs/PACKAGING.md row whose type cell starts
// with `type`, without its backticks.
std::string documentedCommand(const std::string& doc, const std::string& type) {
    const std::string rowStart = "| " + type;
    const size_t row = doc.find(rowStart);
    if (row == std::string::npos) return "";
    const size_t rowEnd = doc.find('\n', row);
    const std::string line = doc.substr(row, rowEnd - row);
    const size_t open = line.rfind("| `");
    if (open == std::string::npos) return "";
    const size_t first = open + 3;
    const size_t close = line.find('`', first);
    return line.substr(first, close - first);
}

} // namespace

static void test_arch_users_upgrade_with_pacman() {
    TEST("upgrade: satellite-bin from the pacman repository upgrades with pacman");
    EXPECT_EQ(std::string(satellite::linux_upgrade::PACMAN),
              std::string("sudo pacman -Syu satellite-bin"));
}

static void test_the_document_names_the_commands_the_updater_shows() {
    TEST("upgrade: docs/PACKAGING.md's update table names the updater's commands");
    const std::string doc = readFile(PACKAGING_MD);
    EXPECT(!doc.empty());
    EXPECT_EQ(documentedCommand(doc, "pacman ("), std::string(satellite::linux_upgrade::PACMAN));
    EXPECT_EQ(documentedCommand(doc, "Snap |"), std::string(satellite::linux_upgrade::SNAP));
    EXPECT_EQ(documentedCommand(doc, "Flatpak |"), std::string(satellite::linux_upgrade::FLATPAK));
}

int main() {
    test_arch_users_upgrade_with_pacman();
    test_the_document_names_the_commands_the_updater_shows();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    if (g_fail > 0) {
        std::cout << "  STATUS: FAIL\n";
        return 1;
    }
    std::cout << "  STATUS: ALL PASSED\n";
    return 0;
}
