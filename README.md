# 🎮 Metin2 Gamepad Support (Hybrid Classic + Controller Mode)

An open-source project that brings full native-like gamepad support and a modernized in-game UI overlay to Metin2 using lightweight proxy DLLs (`dinput8.dll` & `d3d9.dll`)—**without modifying any game pack files (`root`/`locale`).**

## ✨ Key Features

* **🔄 Seamless Hybrid Mode (Gamepad + Classic):** Play entirely with a controller or switch back to keyboard/mouse instantly. Moving your mouse or pressing any key automatically switches to Classic Mode and restores the cursor; touching your controller switches right back to Gamepad Mode.
* **🧭 Smooth 8-Way Movement & Camera:** Jitter-free character movement via the left analog stick (with hysteresis) and free camera control via the right stick (with independent X/Y sensitivity sliders in the F8 menu).
* **🎒 Smart Inventory Grid Navigation:** Opening the inventory automatically snaps the right stick to grid slots—intelligently detecting and skipping over 2-slot and 3-slot items. Use `L1`/`R1` to cycle through inventory pages.
* **⚡ L2 Modifier Layer & Humanized Input:** Hold `L2` to access secondary actions (such as camera zoom `R`/`F` or extra hotkeys). Held actions trigger with randomized, human-like intervals rather than robotic macro timings.
* **🛠 Metin2-Themed In-Game Control Center (`F8`):** Press `F8` anytime in-game to rebind buttons, adjust sensitivities, toggle the live HUD overlay, or send feedback directly without needing external software.

## 📥 Installation

1. Download the latest release `.zip` from the **Releases** section on the right.
2. Extract `dinput8.dll` and `d3d9.dll` directly into your main Metin2 installation folder (next to `metin2client.exe`).
3. Launch the game and press **F8** to customize your controls.

> **⚠️ Disclaimer & Important Note**
> 
> This tool is strictly a quality-of-life input and UI overlay, not a cheat or automation bot. It operates as an external proxy layer (`dinput8.dll` & `d3d9.dll`). While it has been tested actively without triggering anti-cheat issues, **all responsibility regarding potential account restrictions on official or private servers rests with the user.** Using it on an alternate account first is recommended.

## ❤️ Support the Project

If you find this project useful and want to support future updates:
* **Patreon (Global):** [://patreon.com](https://www.://patreon.com)
* **ByNoGame (TR):** [://bynogame.com](https://www.://bynogame.com)



# Metin2-Gamepad-Support
Metin2 için tam kapsamlı Gamepad / Kontrolcü desteği, oyun içi F8 tuş atama paneli ve canlı HUD arayüzü.
<img width="1363" height="767" alt="709975068-0d2d87dc-1cfa-4a9d-898d-3227138a4c0b" src="https://github.com/user-attachments/assets/437253b2-9854-4898-a192-74af0c5ef780" />
<img width="1364" height="767" alt="709973268-106cb227-8287-4257-8ffa-d9c3482926ad" src="https://github.com/user-attachments/assets/ba6aa64d-f863-43c6-8995-3c2d0e17f13c" />


<img width="553" height="728" alt="image" src="https://github.com/user-attachments/assets/527ab1c9-b37a-40d9-8017-5ad5f1a9a47b" /> <img width="293" height="188" alt="image" src="https://github.com/user-attachments/assets/809a1106-d284-4f9d-abbc-2b57ad232814" />


# 🎮 Metin2 Gamepad Support (Klasik Mod + Gamepad Birleşik Kontrol)

Metin2 istemcisine **oyun dosyalarını (root/locale) bozmadan**, harici proxy DLL (`dinput8.dll` & `d3d9.dll`) mimarisiyle tam teşekküllü oyun kolu (Gamepad) ve modernleştirilmiş klasik arayüz desteği ekleyen açık kaynaklı projedir.

## ✨ Öne Çıkan Özellikler

* **🔄 Klasik Mod ve Gamepad Birleşik Çalışma:** Oyunu ister tamamen Gamepad ile oynayın, ister anında klavye/fareye (Klasik Mod) geçin. Fareyi oynattığınızda veya klavyeye bastığınızda sistem otomatik olarak Klasik Mod'a geçer ve imleci geri getirir; kola dokunduğunuz anda tekrar Gamepad moduna döner.
* **🧭 8-Yönlü Histerezisli Hareket & Kamera:** Sol analog ile titremesiz akıcı yürüyüş, sağ analog ile serbest kamera kontrolü (F8 menüsünden ayrı Yatay ve Dikey hassasiyet ayarı).
* **🎒 Akıllı Envanter Grid Navigasyonu:** Envanter açıldığında sağ analog doğrudan slotlar arasında gezinir (2 ve 3 slotluk eşyaları otomatik algılayıp atlar), L1/R1 ile envanter sekmeleri arasında geçiş yapılır.
* **⚡ 2. Katman (L2) Kombinasyonları & İnsansı Basılı Tutma:** L2 tuşuna basılı tutarak ekstra eylemler (kamera yakınlaştırma/uzaklaştırma R/F vb.) kullanabilirsiniz. Basılı tutulan eylemler makrodan ayırt edilemeyecek şekilde insansı rastgele aralıklarla tetiklenir.
* **🛠️ Metin2 Temalı Oyun İçi Kontrol Merkezi (F8):** Harici programa gerek kalmadan oyun içinden `F8` tuşuna basarak tüm tuşları yeniden atayabilir, HUD panelini açıp kapatabilir ve doğrudan oyun içinden geri bildirim gönderebilirsiniz.

## 📥 Kurulum

1. Sağ taraftaki **[Releases](../../releases)** bölümünden en son sürüm `.zip` dosyasını indirin.
2. Arşiv içindeki `dinput8.dll` ve `d3d9.dll` dosyalarını Metin2'nin kurulu olduğu ana klasöre (`metin2client.exe` yanına) atın.
3. Oyunu başlatın ve oyun içinde **`F8`** tuşuna basarak kontrol merkezinizi özelleştirin.

> [!WARNING]
> **⚠️ Önemli Not ve Sorumluluk Reddi**
> 
> Bu araç bir hile değildir; yalnızca kontrolcü desteği sunmak amacıyla oyun dosyalarına harici bir proxy katmanı (`dinput8.dll` & `d3d9.dll`) ekler. Tarafımca **1 haftadır aktif olarak kullanılmakta olup anti-cheat sistemleri tarafından herhangi bir sorunla karşılaşılmamıştır.**
> 
> Yine de üçüncü parti bir katman olduğu için oyunda oluşabilecek olası ban veya yaptırımların **tüm sorumluluğu kullanıcıya aittir.** Risk almamak adına ana (asıl) hesabınız yerine yan hesaplarınızda kullanmanız tavsiye edilir.


## ❤️ Projeye Destek Ol

Projenin geliştirilmesine ve yeni özelliklerin eklenmesine katkıda bulunmak isterseniz:
* **Patreon (Global):** [patreon.com/whoisayata](https://www.patreon.com/c/whoisayata/membership)
* **ByNoGame (TR):** [bynogame.com/tr/destekle/whoisayata](https://www.bynogame.com/tr/destekle/whoisayata)
