#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cstdint>
#include <cmath>

#include "Metin2Assets.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

namespace M2UI {

    class AssetManager {
    private:
        std::unordered_map<std::wstring, Gdiplus::Bitmap*> cache;

        Gdiplus::Bitmap* CreateFromEmbedded(const EmbeddedAsset& asset) {
            if (!asset.px || asset.w <= 0 || asset.h <= 0) return nullptr;
            Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(asset.w, asset.h, PixelFormat32bppARGB);
            Gdiplus::BitmapData bd;
            Gdiplus::Rect rc(0, 0, asset.w, asset.h);
            if (bmp->LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
                uint8_t* dst = static_cast<uint8_t*>(bd.Scan0);
                for (int y = 0; y < asset.h; ++y) {
                    memcpy(dst + y * bd.Stride, &asset.px[y * asset.w], asset.w * 4);
                }
                bmp->UnlockBits(&bd);
            }
            return bmp;
        }


        void Register(const std::wstring& key, const EmbeddedAsset& asset) {
            cache[key] = CreateFromEmbedded(asset);
        }

    public:
        void Init(const std::wstring & = L"") {
            if (!cache.empty()) return;
            Register(L"board_base", emb_board_base);
            Register(L"board_c_lt", emb_board_c_lt);
            Register(L"board_c_rt", emb_board_c_rt);
            Register(L"board_c_lb", emb_board_c_lb);
            Register(L"board_c_rb", emb_board_c_rb);
            Register(L"board_l_t", emb_board_l_t);
            Register(L"board_l_b", emb_board_l_b);
            Register(L"board_l_l", emb_board_l_l);
            Register(L"board_l_r", emb_board_l_r);
            Register(L"tbar_l", emb_tbar_l);
            Register(L"tbar_c", emb_tbar_c);
            Register(L"tbar_r", emb_tbar_r);
            Register(L"tbg_c", emb_tbg_c);
            Register(L"tbg_c_lt", emb_tbg_c_lt);
            Register(L"tbg_c_rt", emb_tbg_c_rt);
            Register(L"tbg_c_lb", emb_tbg_c_lb);
            Register(L"tbg_c_rb", emb_tbg_c_rb);
            Register(L"tbg_l_t", emb_tbg_l_t);
            Register(L"tbg_l_b", emb_tbg_l_b);
            Register(L"tbg_l_l", emb_tbg_l_l);
            Register(L"tbg_l_r", emb_tbg_l_r);

            Register(L"btn_large_1", emb_btn_large_1);
            Register(L"btn_large_2", emb_btn_large_2);
            Register(L"btn_large_3", emb_btn_large_3);
            Register(L"btn_mid_1", emb_btn_mid_1);
            Register(L"btn_mid_2", emb_btn_mid_2);
            Register(L"btn_mid_3", emb_btn_mid_3);
            Register(L"btn_small_1", emb_btn_small_1);
            Register(L"btn_small_2", emb_btn_small_2);
            Register(L"btn_small_3", emb_btn_small_3);
            Register(L"btn_close_1", emb_btn_close_1);
            Register(L"btn_close_2", emb_btn_close_2);
            Register(L"slot_base", emb_slot_base);
            Register(L"slot_eff_1", emb_slot_eff_1);
            Register(L"slot_eff_2", emb_slot_eff_2);
            Register(L"slot_eff_3", emb_slot_eff_3);
            Register(L"tab_1", emb_tab_1);
            Register(L"tab_2", emb_tab_2);
            Register(L"tab_3", emb_tab_3);

            Register(L"donate_icon", emb_donate_icon);
        }

        void Cleanup() {
            for (auto& kv : cache) delete kv.second;
            cache.clear();
        }

        Gdiplus::Bitmap* Get(const std::wstring& key) {
            auto it = cache.find(key);
            return (it != cache.end()) ? it->second : nullptr;
        }

        bool DrawBmp(Gdiplus::Graphics& g, const std::wstring& key, int x, int y, int w, int h, float alpha = 1.0f) {
            Gdiplus::Bitmap* bmp = Get(key);
            if (!bmp) return false;
            Gdiplus::ImageAttributes attr;
            if (alpha < 0.99f) {
                Gdiplus::ColorMatrix cm = {
                    1,0,0,0,0, 0,1,0,0,0, 0,0,1,0,0, 0,0,0,alpha,0, 0,0,0,0,1
                };
                attr.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
            }
            g.DrawImage(bmp, Gdiplus::Rect(x, y, w, h), 0, 0, bmp->GetWidth(), bmp->GetHeight(), Gdiplus::UnitPixel, &attr);
            return true;
        }

        bool DrawTiledButton(Gdiplus::Graphics& g, const std::wstring& key, int x, int y, int w, int h) {
            Gdiplus::Bitmap* bmp = Get(key);
            if (!bmp) return false;
            int sw = bmp->GetWidth(), sh = bmp->GetHeight(), cap = 8;
            if (w <= cap * 2 + 2 || sw <= cap * 2 + 2) {
                g.DrawImage(bmp, Gdiplus::Rect(x, y, w, h), 0, 0, sw, sh, Gdiplus::UnitPixel);
                return true;
            }
            g.DrawImage(bmp, Gdiplus::Rect(x, y, cap, h), 0, 0, cap, sh, Gdiplus::UnitPixel);
            int midW = sw - cap * 2;
            Gdiplus::Region oldClip; g.GetClip(&oldClip);
            g.SetClip(Gdiplus::Rect(x + cap, y, w - cap * 2, h));
            for (int tx = x + cap; tx < x + w - cap; tx += midW) {
                int dw = (tx + midW > x + w - cap) ? (x + w - cap - tx) : midW;
                g.DrawImage(bmp, Gdiplus::Rect(tx, y, dw, h), cap, 0, dw, sh, Gdiplus::UnitPixel);
            }
            g.SetClip(&oldClip);
            g.DrawImage(bmp, Gdiplus::Rect(x + w - cap, y, cap, h), sw - cap, 0, cap, sh, Gdiplus::UnitPixel);
            return true;
        }

        void DrawBoard(Gdiplus::Graphics& g, int x, int y, int w, int h, float alpha = 1.0f) {
            Gdiplus::Bitmap* tl = Get(L"board_c_lt");
            Gdiplus::Bitmap* baseTex = Get(L"board_base");
            if (tl && baseTex) {
                int cs = tl->GetWidth(), bw = baseTex->GetWidth(), bh = baseTex->GetHeight();
                Gdiplus::SolidBrush innerBase(Gdiplus::Color(242, 24, 21, 18));
                g.FillRectangle(&innerBase, x + cs, y + cs, w - cs * 2, h - cs * 2);

                Gdiplus::Region oldClip; g.GetClip(&oldClip);
                g.SetClip(Gdiplus::Rect(x + cs, y + cs, w - cs * 2, h - cs * 2));
                for (int ty = y + cs; ty < y + h - cs; ty += bh)
                    for (int tx = x + cs; tx < x + w - cs; tx += bw)
                        DrawBmp(g, L"board_base", tx, ty, bw, bh, alpha);

                int segW = Get(L"board_l_t") ? Get(L"board_l_t")->GetWidth() : cs;
                g.SetClip(Gdiplus::Rect(x + cs, y, w - cs * 2, cs));
                for (int tx = x + cs; tx < x + w - cs; tx += segW) DrawBmp(g, L"board_l_t", tx, y, segW, cs, alpha);
                g.SetClip(Gdiplus::Rect(x + cs, y + h - cs, w - cs * 2, cs));
                for (int tx = x + cs; tx < x + w - cs; tx += segW) DrawBmp(g, L"board_l_b", tx, y + h - cs, segW, cs, alpha);

                int segH = Get(L"board_l_l") ? Get(L"board_l_l")->GetHeight() : cs;
                g.SetClip(Gdiplus::Rect(x, y + cs, cs, h - cs * 2));
                for (int ty = y + cs; ty < y + h - cs; ty += segH) DrawBmp(g, L"board_l_l", x, ty, cs, segH, alpha);
                g.SetClip(Gdiplus::Rect(x + w - cs, y + cs, cs, h - cs * 2));
                for (int ty = y + cs; ty < y + h - cs; ty += segH) DrawBmp(g, L"board_l_r", x + w - cs, ty, cs, segH, alpha);
                g.SetClip(&oldClip);

                DrawBmp(g, L"board_c_lt", x, y, cs, cs, alpha);
                DrawBmp(g, L"board_c_rt", x + w - cs, y, cs, cs, alpha);
                DrawBmp(g, L"board_c_lb", x, y + h - cs, cs, cs, alpha);
                DrawBmp(g, L"board_c_rb", x + w - cs, y + h - cs, cs, cs, alpha);
            }
        }

        void DrawCloseIconButton(Gdiplus::Graphics& g, int x, int y, bool isHover = false) {
            std::wstring k = isHover ? L"btn_close_2" : L"btn_close_1";
            Gdiplus::Bitmap* b = Get(k);
            int w = b ? b->GetWidth() : 15, h = b ? b->GetHeight() : 15;
            DrawBmp(g, k, x, y, w, h, 1.0f);
        }

        void DrawTitleBar(Gdiplus::Graphics& g, int x, int y, int w, int h, const std::wstring& title, bool closeHover = false) {
            Gdiplus::Bitmap* tL = Get(L"tbar_l");
            Gdiplus::Bitmap* tC = Get(L"tbar_c");
            Gdiplus::Bitmap* tR = Get(L"tbar_r");
            if (tL && tC && tR) {
                int lw = tL->GetWidth(), rw = tR->GetWidth(), th = tL->GetHeight(), cw = tC->GetWidth();
                DrawBmp(g, L"tbar_l", x, y, lw, th, 1.0f);
                Gdiplus::Region oldClip; g.GetClip(&oldClip);
                g.SetClip(Gdiplus::Rect(x + lw, y, w - lw - rw, th));
                for (int tx = x + lw; tx < x + w - rw; tx += cw) DrawBmp(g, L"tbar_c", tx, y, cw, th, 1.0f);
                g.SetClip(&oldClip);
                DrawBmp(g, L"tbar_r", x + w - rw, y, rw, th, 1.0f);
                h = th;
            }
            DrawM2Text(g, title, (float)(x + 12), (float)(y - 2), (float)(w - 40), (float)h, 12.0f,
                Gdiplus::Color(255, 255, 244, 224), Gdiplus::StringAlignmentCenter, false);
            DrawCloseIconButton(g, x + w - 21, y + 4, closeHover);
        }

        void DrawInventoryStyleTab(Gdiplus::Graphics& g, int x, int y, int w, int h, const std::wstring& label,
            bool isActive = false, bool isHover = false) {
            std::wstring k = isActive ? L"tab_3" : (isHover ? L"tab_2" : L"tab_1");
            if (!DrawTiledButton(g, k, x, y, w, h)) {
                Gdiplus::SolidBrush outerFrame(Gdiplus::Color(255, 12, 11, 10));
                g.FillRectangle(&outerFrame, x, y, w, h);

                Gdiplus::SolidBrush bodyBrush(isActive
                    ? Gdiplus::Color(255, 128, 118, 106)
                    : (isHover ? Gdiplus::Color(255, 94, 86, 76) : Gdiplus::Color(255, 76, 69, 61)));
                g.FillRectangle(&bodyBrush, x + 2, y + 2, w - 4, h - 4);

                Gdiplus::Pen lightPen(isActive ? Gdiplus::Color(255, 224, 216, 204) : Gdiplus::Color(255, 164, 154, 140), 1.0f);
                g.DrawLine(&lightPen, x + 1, y + 1, x + w - 2, y + 1);
                g.DrawLine(&lightPen, x + 1, y + 1, x + 1, y + h - 2);

                Gdiplus::Pen darkPen(Gdiplus::Color(255, 34, 30, 26), 1.0f);
                g.DrawLine(&darkPen, x + 1, y + h - 2, x + w - 2, y + h - 2);
                g.DrawLine(&darkPen, x + w - 2, y + 1, x + w - 2, y + h - 2);

                Gdiplus::Pen innerBox(isActive ? Gdiplus::Color(255, 178, 168, 154) : Gdiplus::Color(255, 106, 98, 88), 1.0f);
                g.DrawRectangle(&innerBox, x + 3, y + 3, w - 7, h - 7);
            }
            Gdiplus::Color txtCol = isActive ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 228, 222, 212);
            DrawM2Text(g, label, (float)x, (float)(y - 1), (float)w, (float)h, 12.0f, txtCol, Gdiplus::StringAlignmentCenter, false);
        }

        void DrawM2Button(Gdiplus::Graphics& g, int x, int y, int w, int h, const std::wstring& label,
            bool isActive = false, bool isHover = false, Gdiplus::Color customTextCol = Gdiplus::Color(0, 0, 0, 0)) {
            std::wstring prefix = L"btn_mid_";
            if (w >= 95) prefix = L"btn_large_";
            else if (w <= 42) prefix = L"btn_small_";
            std::wstring suffix = isActive ? L"3" : (isHover ? L"2" : L"1");
            DrawTiledButton(g, prefix + suffix, x, y, w, h);

            Gdiplus::Color tCol = (customTextCol.GetA() > 0)
                ? customTextCol
                : (isActive ? Gdiplus::Color(255, 255, 225, 110) : Gdiplus::Color(255, 245, 242, 235));
            DrawM2Text(g, label, (float)(x + 2), (float)(y + (isActive ? 0 : -1)), (float)(w - 4), (float)h, 12.0f, tCol, Gdiplus::StringAlignmentCenter, false);
        }
        // Buton disinda bagimsiz buyuk ikon cizimi (or. bagis butonu yanina 34x34)
        void DrawStandaloneIcon(Gdiplus::Graphics& g, int x, int y, int size, const std::wstring& iconKey) {
            Gdiplus::Bitmap* icon = Get(iconKey);
            if (!icon) return;
            Gdiplus::InterpolationMode prev = g.GetInterpolationMode();
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(icon, Gdiplus::Rect(x, y, size, size), 0, 0, icon->GetWidth(), icon->GetHeight(), Gdiplus::UnitPixel);
            g.SetInterpolationMode(prev);
        }

        void DrawM2IconButton(Gdiplus::Graphics& g, int x, int y, int w, int h,
            const std::wstring& iconKey, const std::wstring& label,
            bool isHover = false, Gdiplus::Color textCol = Gdiplus::Color(255, 255, 220, 95)) {
            DrawTiledButton(g, isHover ? L"btn_large_2" : L"btn_large_1", x, y, w, h);

            Gdiplus::Bitmap* icon = Get(iconKey);
            int iconSize = h - 3;
            int ix = x + 8;
            int iy = y + (h - iconSize) / 2;

            if (icon) {
                Gdiplus::InterpolationMode prev = g.GetInterpolationMode();
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                g.DrawImage(icon, Gdiplus::Rect(ix, iy, iconSize, iconSize), 0, 0, icon->GetWidth(), icon->GetHeight(), Gdiplus::UnitPixel);
                g.SetInterpolationMode(prev);
            }
            else {
                // Fallback: Altin sikke ikonu
                Gdiplus::SolidBrush coinBg(Gdiplus::Color(255, 235, 185, 45));
                Gdiplus::Pen coinBorder(Gdiplus::Color(255, 255, 240, 150), 1.2f);
                g.FillEllipse(&coinBg, ix + 2, iy + 2, iconSize - 4, iconSize - 4);
                g.DrawEllipse(&coinBorder, ix + 2, iy + 2, iconSize - 4, iconSize - 4);
            }

            int textOffsetX = iconSize + 6;
            DrawM2Text(g, label, (float)(x + textOffsetX), (float)(y - 1), (float)(w - textOffsetX - 4), (float)h, 12.0f, textCol, Gdiplus::StringAlignmentCenter, false);
        }

        void DrawThinBoardGold(Gdiplus::Graphics& g, int x, int y, int w, int h, float alpha = 0.88f) {
            Gdiplus::Bitmap* tl = Get(L"tbg_c_lt");
            if (!tl) {
                Gdiplus::SolidBrush bg(Gdiplus::Color((BYTE)(215 * alpha), 14, 12, 10));
                Gdiplus::Pen borderOuter(Gdiplus::Color((BYTE)(240 * alpha), 156, 126, 68), 1.5f);
                Gdiplus::Pen borderInner(Gdiplus::Color((BYTE)(180 * alpha), 78, 60, 30), 1.0f);
                g.FillRectangle(&bg, x, y, w, h);
                g.DrawRectangle(&borderOuter, x, y, w, h);
                g.DrawRectangle(&borderInner, x + 2, y + 2, w - 4, h - 4);
                return;
            }
            int cs = tl->GetWidth();
            DrawBmp(g, L"tbg_c", x + cs, y + cs, w - cs * 2, h - cs * 2, alpha);
            DrawBmp(g, L"tbg_l_t", x + cs, y, w - cs * 2, cs, alpha);
            DrawBmp(g, L"tbg_l_b", x + cs, y + h - cs, w - cs * 2, cs, alpha);
            DrawBmp(g, L"tbg_l_l", x, y + cs, cs, h - cs * 2, alpha);
            DrawBmp(g, L"tbg_l_r", x + w - cs, y + cs, cs, h - cs * 2, alpha);
            DrawBmp(g, L"tbg_c_lt", x, y, cs, cs, alpha);
            DrawBmp(g, L"tbg_c_rt", x + w - cs, y, cs, cs, alpha);
            DrawBmp(g, L"tbg_c_lb", x, y + h - cs, cs, cs, alpha);
            DrawBmp(g, L"tbg_c_rb", x + w - cs, y + h - cs, cs, cs, alpha);
        }

        void DrawM2Text(Gdiplus::Graphics& g, const std::wstring& text, float x, float y, float w, float h,
            float fontSize = 12.0f, Gdiplus::Color color = Gdiplus::Color(255, 245, 230, 184),
            Gdiplus::StringAlignment align = Gdiplus::StringAlignmentNear, bool bold = false) {
            Gdiplus::TextRenderingHint prevHint = g.GetTextRenderingHint();
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintSingleBitPerPixelGridFit);

            Gdiplus::FontFamily ff(L"Tahoma");
            Gdiplus::Font font(&ff, fontSize, bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::StringFormat fmt;
            fmt.SetAlignment(align);
            fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

            Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(255, 0, 0, 0));
            Gdiplus::RectF rShadow(x + 1.0f, y + 1.0f, w, h);
            g.DrawString(text.c_str(), -1, &font, rShadow, &fmt, &shadowBrush);

            Gdiplus::SolidBrush textBrush(color);
            Gdiplus::RectF rText(x, y, w, h);
            g.DrawString(text.c_str(), -1, &font, rText, &fmt, &textBrush);
            g.SetTextRenderingHint(prevHint);
        }

        void DrawModernLStick(Gdiplus::Graphics& g, int cx, int cy, float lx, float ly) {
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush ringBg(Gdiplus::Color(220, 22, 18, 14));
            Gdiplus::Pen ringPen(Gdiplus::Color(235, 165, 134, 72), 1.5f);
            g.FillEllipse(&ringBg, cx - 22, cy - 22, 44, 44);
            g.DrawEllipse(&ringPen, cx - 22, cy - 22, 44, 44);

            float mag = sqrtf(lx * lx + ly * ly);
            bool active = (mag > 0.15f);
            for (int i = 0; i < 8; i++) {
                float ang = i * (3.14159265f / 4.0f);
                float dx = cosf(ang), dy = sinf(ang);
                int px = cx + (int)roundf(dx * 16.0f), py = cy + (int)roundf(dy * 16.0f);
                bool lit = active && (((lx / mag) * dx + (ly / mag) * dy) > 0.72f);
                Gdiplus::SolidBrush dotBr(lit ? Gdiplus::Color(255, 255, 215, 0) : Gdiplus::Color(130, 135, 115, 80));
                int r = lit ? 3 : 2;
                g.FillEllipse(&dotBr, px - r, py - r, r * 2, r * 2);
            }
            int kx = cx + (int)roundf(lx * 7.0f), ky = cy + (int)roundf(ly * 7.0f);
            Gdiplus::SolidBrush knobBg(active ? Gdiplus::Color(255, 195, 152, 58) : Gdiplus::Color(240, 58, 48, 34));
            Gdiplus::Pen knobPen(active ? Gdiplus::Color(255, 255, 235, 130) : Gdiplus::Color(220, 175, 145, 85), 1.3f);
            g.FillEllipse(&knobBg, kx - 8, ky - 8, 16, 16);
            g.DrawEllipse(&knobPen, kx - 8, ky - 8, 16, 16);
            g.SetSmoothingMode(Gdiplus::SmoothingModeDefault);
        }

        void DrawActionButton(Gdiplus::Graphics& g, int x, int y, int w, int h,
            const std::wstring& btnLabel, const std::wstring& actionDesc, bool isPressed) {
            DrawTiledButton(g, isPressed ? L"btn_small_3" : L"btn_small_1", x, y, w, h);
            Gdiplus::Color keyCol = isPressed ? Gdiplus::Color(255, 255, 235, 110) : Gdiplus::Color(255, 255, 215, 0);
            DrawM2Text(g, btnLabel, (float)x, (float)(y + (isPressed ? 0 : -1)), (float)w, (float)h, 11.5f, keyCol, Gdiplus::StringAlignmentCenter, false);
            if (!actionDesc.empty()) {
                Gdiplus::Color descCol = isPressed ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 245, 230, 184);
                DrawM2Text(g, actionDesc, (float)(x + w + 8), (float)(y - 1), 130.0f, (float)h, 12.0f, descCol, Gdiplus::StringAlignmentNear, false);
            }
        }

        void DrawCleanSlotBox(Gdiplus::Graphics& g, int x, int y, int size,
            HDC hdcGameCapture, int srcSlotX, int srcSlotY, bool hasValidCapture, bool isPressed) {
            DrawBmp(g, L"slot_base", x, y, size, size, 0.95f);
            if (hasValidCapture && hdcGameCapture && srcSlotX >= 0 && srcSlotY >= 0) {
                HDC hdcGdi = g.GetHDC();
                StretchBlt(hdcGdi, x + 1, y + 1, size - 2, size - 2, hdcGameCapture, srcSlotX + 1, srcSlotY + 1, 30, 30, SRCCOPY);
                g.ReleaseHDC(hdcGdi);
            }
            Gdiplus::Pen borderPen(isPressed ? Gdiplus::Color(255, 255, 225, 40) : Gdiplus::Color(235, 165, 134, 72), isPressed ? 2.0f : 1.2f);
            g.DrawRectangle(&borderPen, x, y, size, size);
            if (isPressed) {
                Gdiplus::SolidBrush pressGlow(Gdiplus::Color(65, 255, 215, 0));
                g.FillRectangle(&pressGlow, x, y, size, size);
            }
        }

        void DrawSlotActiveEffect(Gdiplus::Graphics& g, int x, int y, int slotW, int slotH, int slotSpan) {
            int span = (slotSpan < 1) ? 1 : ((slotSpan > 3) ? 3 : slotSpan);
            int totalH = slotH * span;

            std::wstring k = (span == 2) ? L"slot_eff_2" : ((span == 3) ? L"slot_eff_3" : L"slot_eff_1");
            // Eger 2 veya 3 slotluk doku yoksa 1 slotluk dokuyu tam yukseklige yay
            if (!DrawBmp(g, k, x - 2, y - 2, slotW + 4, totalH + 4, 0.95f)) {
                DrawBmp(g, L"slot_eff_1", x - 2, y - 2, slotW + 4, totalH + 4, 0.95f);
            }

            // Yeni esya efektinden ayirt etmek icin belirgin altin vurgu cercevesi + 4 kose nisangahi
            Gdiplus::SolidBrush softTint(Gdiplus::Color(38, 255, 215, 40));
            g.FillRectangle(&softTint, x + 1, y + 1, slotW - 2, totalH - 2);

            Gdiplus::Pen outerGold(Gdiplus::Color(235, 255, 215, 0), 1.5f);
            g.DrawRectangle(&outerGold, x, y, slotW - 1, totalH - 1);

            // 4 koseye parlak beyaz-altin L kosebent (hedef kilidi gorunumu)
            Gdiplus::Pen cornerPen(Gdiplus::Color(255, 255, 250, 190), 2.0f);
            int cLen = 7;
            // Sol-Ust
            g.DrawLine(&cornerPen, x, y, x + cLen, y);
            g.DrawLine(&cornerPen, x, y, x, y + cLen);
            // Sag-Ust
            g.DrawLine(&cornerPen, x + slotW - 1 - cLen, y, x + slotW - 1, y);
            g.DrawLine(&cornerPen, x + slotW - 1, y, x + slotW - 1, y + cLen);
            // Sol-Alt
            g.DrawLine(&cornerPen, x, y + totalH - 1 - cLen, x, y + totalH - 1);
            g.DrawLine(&cornerPen, x, y + totalH - 1, x + cLen, y + totalH - 1);
            // Sag-Alt
            g.DrawLine(&cornerPen, x + slotW - 1 - cLen, y + totalH - 1, x + slotW - 1, y + totalH - 1);
            g.DrawLine(&cornerPen, x + slotW - 1, y + totalH - 1 - cLen, x + slotW - 1, y + totalH - 1);
        }
    };
}