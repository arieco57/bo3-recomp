// On-screen performance overlay: a few text lines rasterized on the CPU with a tiny 5x7 font
// into a small image, copied into the swapchain image (top-left) at present time.
#include "overlay.h"
#include "vk.h"
#include "../log.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
// 5x7 glyphs, one string of 7 rows of 5 chars ('#' = on) per character.
struct Glyph { char c; const char* rows; };
const Glyph kFont[] = {
    { '0', " ### #   ##  ### # ###  ##   # ### " }, { '1', "  #   ##    #    #    #    #   ### " },
    { '2', " ### #   #    #   #   #   #   #####" }, { '3', "#####   #   #     #     ##   # ### " },
    { '4', "   #   ##  # # #  # #####   #    # " }, { '5', "######    ####     #    ##   # ### " },
    { '6', "  ##  #   #    #### #   ##   # ### " }, { '7', "#####    #   #   #   #    #    #   " },
    { '8', " ### #   ##   # ### #   ##   # ### " }, { '9', " ### #   ##   # ####    #   #  ##  " },
    { 'A', " ### #   ##   #######   ##   ##   #" }, { 'B', "#### #   ##   ##### #   ##   ##### " },
    { 'C', " ### #   ##    #    #    #   # ### " }, { 'D', "#### #   ##   ##   ##   ##   ##### " },
    { 'E', "######    #    #### #    #    #####" }, { 'F', "######    #    #### #    #    #    " },
    { 'G', " ### #   ##    # ####   ##   # ####" }, { 'H', "#   ##   ##   #######   ##   ##   #" },
    { 'I', " ###   #    #    #    #    #   ### " }, { 'J', "  ###   #    #    #    # #  #  ##  " },
    { 'K', "#   ##  # # #  ##   # #  #  # #   #" }, { 'L', "#    #    #    #    #    #    #####" },
    { 'M', "#   ### ### # ##   ##   ##   ##   #" }, { 'N', "#   ###  ## # ##  ###   ##   ##   #" },
    { 'O', " ### #   ##   ##   ##   ##   # ### " }, { 'P', "#### #   ##   ##### #    #    #    " },
    { 'Q', " ### #   ##   ##   ## # ##  #  ## #" }, { 'R', "#### #   ##   ##### # #  #  # #   #" },
    { 'S', " #####    #     ###     #    ##### " }, { 'T', "#####  #    #    #    #    #    #  " },
    { 'U', "#   ##   ##   ##   ##   ##   # ### " }, { 'V', "#   ##   ##   ##   ##   # # #   #  " },
    { 'W', "#   ##   ##   ## # ## # ## # # # # " }, { 'X', "#   ##   # # #   #   # # #   ##   #" },
    { 'Y', "#   ##   # # #   #    #    #    #  " }, { 'Z', "#####    #   #   #   #   #    #####" },
    { '.', "                              ##   " }, { ':', "      ##             ##            " },
    { '%', "##   ##  #   #   #   #   #  ##   ##" }, { '/', "    #    #   #   #   #    #    #   " },
    { '(', "  #   #   #    #    #     #     #  " }, { ')', " #     #     #    #    #   #   #   " },
    { '-', "               ###                 " }, { '+', "       #    #  #####  #    #       " },
    { 'x', "          #   # # #   #   # # #   #" }, { ' ', "                                   " },
};

const char* GlyphRows(char c)
{
    if (c >= 'a' && c <= 'z' && c != 'x') c = char(c - 32);
    for (auto& g : kFont) if (g.c == c) return g.rows;
    return kFont[std::size(kFont) - 1].rows;
}

constexpr uint32_t kCharW = 6, kCharH = 9, kMaxCols = 48, kMaxRows = 6;
constexpr uint32_t kImgW = kMaxCols * kCharW + 4, kImgH = kMaxRows * kCharH + 4;

VkImageObj s_img;
VkBufferObj s_buf;
uint32_t s_w = 0, s_h = 0;   // used part
std::vector<std::string> s_lines;
bool s_dirty = false;
}

void OverlaySetText(const std::vector<std::string>& lines)
{
    s_lines = lines;
    if (s_lines.size() > kMaxRows) s_lines.resize(kMaxRows);
    s_dirty = true;
}

void OverlayRecord(VkCommandBuffer cmd, VkImage target, VkExtent2D extent)
{
    if (s_lines.empty()) return;
    if (!s_img.image)
    {
        s_img = VkCreateImage(kImgW, kImgH, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        s_buf = VkCreateBuffer(uint64_t(kImgW) * kImgH * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    }
    if (s_dirty)
    {
        // (the previous present that used the buffer finished: the caller waited for its fence)
        s_dirty = false;
        size_t cols = 0;
        for (auto& l : s_lines) cols = std::max(cols, std::min<size_t>(l.size(), kMaxCols));
        s_w = uint32_t(cols) * kCharW + 4;
        s_h = uint32_t(s_lines.size()) * kCharH + 3;
        uint32_t* px = static_cast<uint32_t*>(s_buf.mapped);
        for (uint32_t i = 0; i < kImgW * kImgH; i++) px[i] = 0xFF101010;   // dark background (BGRA)
        for (size_t row = 0; row < s_lines.size(); row++)
        {
            uint32_t color = row == 0 ? 0xFF40FF40 : 0xFFE0E0E0;   // first line green
            const std::string& l = s_lines[row];
            for (size_t col = 0; col < l.size() && col < kMaxCols; col++)
            {
                const char* g = GlyphRows(l[col]);
                for (uint32_t y = 0; y < 7; y++)
                    for (uint32_t x = 0; x < 5; x++)
                        if (g[y * 5 + x] == '#') px[(2 + row * kCharH + y) * kImgW + 2 + col * kCharW + x] = color;
            }
        }
        VkTransition(cmd, s_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy bic{};
        bic.bufferRowLength = kImgW;
        bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        bic.imageExtent = { kImgW, kImgH, 1 };
        vkCmdCopyBufferToImage(cmd, s_buf.buffer, s_img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    }
    VkTransition(cmd, s_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    // scale 2x on screens of 1080p and up
    int sc = extent.height >= 1000 ? 3 : 2;
    VkImageBlit b{};
    b.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    b.srcOffsets[1] = { int(s_w), int(s_h), 1 };
    b.dstSubresource = b.srcSubresource;
    b.dstOffsets[0] = { 8, 8, 0 };
    b.dstOffsets[1] = { std::min(8 + int(s_w) * sc, int(extent.width)), std::min(8 + int(s_h) * sc, int(extent.height)), 1 };
    vkCmdBlitImage(cmd, s_img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
}

// ---------------------------------------------------------------- options button
namespace
{
constexpr const char* kButtonText = "OPCIONES";
constexpr uint32_t kBtnW = 8 * kCharW + 9, kBtnH = kCharH + 8;
VkImageObj s_btn;
VkBufferObj s_btnBuf;
std::atomic<float> s_btnRect[4] = { 0.9f, 0.0f, 1.0f, 0.1f };
}

void OverlayButtonRecord(VkCommandBuffer cmd, VkImage target, VkExtent2D extent)
{
    if (!s_btn.image)
    {
        s_btn = VkCreateImage(kBtnW, kBtnH, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        s_btnBuf = VkCreateBuffer(uint64_t(kBtnW) * kBtnH * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        uint32_t* px = static_cast<uint32_t*>(s_btnBuf.mapped);
        for (uint32_t y = 0; y < kBtnH; y++)
            for (uint32_t x = 0; x < kBtnW; x++)
            {
                bool border = x == 0 || y == 0 || x == kBtnW - 1 || y == kBtnH - 1;
                px[y * kBtnW + x] = border ? 0xFFC0C0C0 : 0xFF202020;
            }
        for (size_t col = 0; kButtonText[col]; col++)
        {
            const char* g = GlyphRows(kButtonText[col]);
            for (uint32_t y = 0; y < 7; y++)
                for (uint32_t x = 0; x < 5; x++)
                    if (g[y * 5 + x] == '#') px[(5 + y) * kBtnW + 5 + col * kCharW + x] = 0xFFFFFFFF;
        }
        VkTransition(cmd, s_btn, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy bic{};
        bic.bufferRowLength = kBtnW;
        bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        bic.imageExtent = { kBtnW, kBtnH, 1 };
        vkCmdCopyBufferToImage(cmd, s_btnBuf.buffer, s_btn.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    }
    VkTransition(cmd, s_btn, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    // about 1/16 of the screen height, finger-sized on a phone
    int sc = std::max(2, int(extent.height / (kBtnH * 16)));
    int w = int(kBtnW) * sc, h = int(kBtnH) * sc, margin = std::max(8, int(extent.height / 60));
    int x0 = std::max(0, int(extent.width) - w - margin), y0 = margin;
    VkImageBlit b{};
    b.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    b.srcOffsets[1] = { int(kBtnW), int(kBtnH), 1 };
    b.dstSubresource = b.srcSubresource;
    b.dstOffsets[0] = { x0, y0, 0 };
    b.dstOffsets[1] = { std::min(x0 + w, int(extent.width)), std::min(y0 + h, int(extent.height)), 1 };
    vkCmdBlitImage(cmd, s_btn.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    // hit area a bit larger than the drawing
    float W = float(extent.width), H = float(extent.height), pad = float(margin);
    s_btnRect[0] = std::max(0.0f, (x0 - pad) / W); s_btnRect[1] = 0.0f;
    s_btnRect[2] = 1.0f; s_btnRect[3] = std::min(1.0f, (y0 + h + pad) / H);
}

void OverlayButtonRect(float r[4])
{
    for (int i = 0; i < 4; i++) r[i] = s_btnRect[i].load();
}
