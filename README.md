# IKEA BILRESA E2489 連接 XIAO ESP32-C6：直接控制板上 LED

使用 **IKEA BILRESA 雙按鍵遙控器 E2489（貨號 606.178.77）**，透過 Zigbee 控制 **Seeed Studio XIAO ESP32-C6** 的板上 LED。

這個專案已實測完成遙控器入網、授權，以及 LED 開啟、關閉、再次開啟。C6 同時負責建立 Zigbee 網路與控制 LED，控制流程不經過 Home Assistant、MQTT 或另一塊開發板。電腦負責燒錄程式與顯示紀錄。

```text
IKEA BILRESA E2489
        │ Zigbee 開／關指令
        ▼
Seeed Studio XIAO ESP32-C6
  Zigbee 協調器 ＋ LED 燈端點
        │
        ▼
   板上 LED 開／關
```

## 準備物品

| 項目 | 本教學使用的型號 |
| --- | --- |
| 遙控器 | IKEA BILRESA 雙按鍵款，E2489，貨號 606.178.77 |
| 開發板 | Seeed Studio XIAO ESP32-C6 |
| USB 線 | 可傳輸資料的 USB 線 |
| 電腦 | 本教學的燒錄指令使用 Windows PowerShell |
| Python | 實測使用 Python 3.11 |

使用板上 LED，不需要外接 LED 或電阻。本教學的功能是 **LED 開／關**。

## 1. 下載並上傳程式

在此 GitHub 頁面點選 **Code → Download ZIP**，解壓縮後進入專案資料夾。`firmware/` 已包含實測使用的韌體，可直接燒錄。

將 XIAO ESP32-C6 接上電腦，在 Windows 裝置管理員確認序列埠。下方以本次實測的 **COM24** 為例；請換成你電腦上的埠號。

先關閉正在使用該序列埠的監控工具，再於專案資料夾開啟 PowerShell：

```powershell
python -m pip install esptool==5.0.2 pyserial==3.5
```

上傳韌體：

```powershell
python -m esptool --chip esp32c6 --port COM24 --baud 460800 write-flash --flash-mode dio --flash-freq 80m --flash-size 4MB 0x0 firmware/bootloader.bin 0x8000 firmware/partitions.bin 0xf000 firmware/ota_data_initial.bin 0x20000 firmware/firmware.bin
```

燒錄成功會顯示 `Hash of data verified.`。這個命令會覆寫板上的應用程式與分區表。

## 2. 開啟配對紀錄網頁

在同一個專案資料夾執行：

```powershell
python serial_dashboard.py --port COM24 --reset-on-connect
```

瀏覽器開啟 **http://127.0.0.1:8766/**。

啟動後 C6 會建立或還原自己的 Zigbee 網路，並開放 **180 秒**讓遙控器加入。網頁可看到目前狀態、倒數秒數、入網紀錄與開關更新次數。

先確認紀錄出現：

```text
JOIN_READY duration_s=180 channel=11 group=0x549a
GROUP_VERIFIED group=0x549a status=0x00 ready=1
```

這代表 C6 已開放入網，LED 端點也已加入遙控器使用的群組。

> 網頁的「重新掃描」按鈕會重啟 C6，重新開放 180 秒入網時間，並保留已建立的網路資料。若倒數已結束，先按這個按鈕，再進行下一步。

## 3. 遙控器配對：先短按 4 次，再短按 8 次

以下操作都使用 **電池盒內的 system 系統按鈕**，不是遙控器正面的兩個控制按鈕。請先打開電池蓋，將遙控器放在 C6 旁邊。

### A. 重置遙控器

按住 system 按鈕約 **10 秒**，直到紅燈停止閃爍後放開。這會清除遙控器原本的連線設定。

### B. 快速短按 4 次

快速按下並放開 system 按鈕 **4 次**。

遙控器會開始 **橘燈快閃**，進入 Touchlink 模式。

**此時還沒完成本專案需要的配對，請繼續下一步。**

### C. 在橘燈快閃時，再快速短按 8 次

看到橘燈快閃後，在這個模式下立即再連續短按 system 按鈕 **8 次**。

這次實測的現象是 **指示燈熄滅**，遙控器接著以一般 Zigbee 模式加入 C6 的網路。

按鍵順序如下：

```text
長按約 10 秒 → 紅燈停止後放開
            ↓
快速短按 4 次 → 看到橘燈快閃
            ↓
再快速短按 8 次 → 指示燈熄滅
            ↓
等待網頁出現裝置入網與授權成功紀錄
```

**4 次和 8 次分成兩段操作：先看見橘燈快閃，再按後面的 8 次。**

## 4. 確認成功，再測正面按鍵

指示燈熄滅後，等待網頁出現下列紀錄：

| 紀錄 | 意義 |
| --- | --- |
| `DEVICE_ANNOUNCED` | 收到裝置入網宣告 |
| `DEVICE_AUTHORIZED` 且 `status=0` | 授權成功 |
| `Set On/Off: 1` | LED 開啟 |
| `Set On/Off: 0` | LED 關閉 |

接著按遙控器正面的開／關按鈕，確認板上 LED 隨之切換。網頁狀態會顯示「已收到開關控制」，開關更新次數也會增加。

本次實測順序為：**入網 → 授權成功 → LED 開啟 → LED 關閉 → LED 再次開啟**。

## 如果停在橘燈快閃

本次測試曾停在這個狀態，log 持續出現 `SCAN_REQUEST`，但沒有 `DEVICE_ANNOUNCED`。

確認 C6 的入網倒數仍在進行，再依上面的步驟操作：**短按 4 次看到橘燈快閃後，再短按 8 次**。本次成功切換後，遙控器的指示燈熄滅，接著出現入網與授權紀錄。

不要只用橘燈閃爍或熄滅判斷配對成功；以 **入網紀錄、授權成功與實際 LED 開關**確認結果。

## 程式如何運作

| 設定 | 本專案使用值 |
| --- | --- |
| C6 角色 | Zigbee Coordinator（協調器）＋ On/Off 燈 |
| Zigbee 頻道 | 11 |
| 燈端點 | 1 |
| 接收群組 | `0x549A`，十進位 21658 |
| On/Off Cluster | `0x0006` |
| On/Off Attribute | `0x0000` |
| 板上 LED | GPIO15，低電位亮 |
| USB 序列速率 | 115200 baud |

C6 建立網路後，將自己的燈端點加入 `0x549A`。收到開／關指令時，程式更新 GPIO15，控制板上 LED。序列紀錄程式只讀取狀態並提供本機網頁，不負責轉送遙控器的開關指令。

## 檔案位置

- [`firmware/`](firmware/)：已編譯韌體與分區表。
- [`main/touchlink_target.c`](main/touchlink_target.c)：目前的協調器、入網紀錄、群組設定與 LED 控制程式；檔名沿用開發初期名稱。
- [`main/light_driver.c`](main/light_driver.c)：板上 LED 控制。
- [`serial_dashboard.py`](serial_dashboard.py)：序列紀錄與本機網頁。
- [`platformio.ini`](platformio.ini)：建置設定。
- [`partitions.csv`](partitions.csv)：分區表。

程式原始碼與韌體一起提供；只要按照前面的燒錄步驟即可開始配對。實測建置環境為 pioarduino espressif32 54.3.21、ESP-IDF 5.4.2、esp-zigbee-lib 2.0.4。

## 參考資料

- [IKEA BILRESA 606.178.77 商品頁](https://www.ikea.com.tw/zh/products/electronics/connectivity-and-control/bilresa-art-60617877)
- [Zigbee2MQTT：E2489 的配對步驟與群組說明](https://www.zigbee2mqtt.io/devices/E2489.html)
- [Espressif：協調器與 On/Off Light 範例](https://github.com/espressif/esp-zigbee-sdk/tree/606baf47f050684832d189c4cf9837fffe54c052/examples/home_automation_devices/on_off_light)
- [Seeed：XIAO ESP32-C6 文件](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/)

實測日期：2026-09-24。本文記錄的組合為 **BILRESA E2489（606.178.77）＋ Seeed Studio XIAO ESP32-C6**。
