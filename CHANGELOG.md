# 更新日誌（Changelog）

本文件記錄 **A6400 Extra-Menu** 各版本在 Sony ILCE-6400（FW `Ver. 2.00`）實機與主機端的功能演進、效能調校、重大問題修復與驗證紀錄。

---

## [V3.9 SSLF] - 2026-11-20（Sony Super LUT FILM）

> 實機驗收基準：`package=7ec416fa937fab67`（`194,560 B <= 196,608 B`）

### 新增功能（Added）
- **免依賴藍牙開機自動掛載 `/tmp_bt` ＋ `/etc/a6400-autoexec.so`**：
  - [`tools/build_autoexec_hook.py`](tools/build_autoexec_hook.py) 與 [`host-tests/film-runtime-v2-templates/bootstrap-local.sh`](host-tests/film-runtime-v2-templates/bootstrap-local.sh) 在 `/tmp_bt` 仍為唯讀根目錄時自動掛載 `size=2048k tmpfs`，建立鎖失敗時於 `/tmp/a6400-hook-err.log` 寫入單行診斷（無須再強迫維持 `Bluetooth 功能 = 開`）。
- **多組自訂快門音切換與 SD 卡安全覆寫**：
  - [`camera/charo_bind.c`](camera/charo_bind.c) 擴充支援三組 `MS_BIND` 可選槽位（`0x33 Dslr_Emnt_WPS_Connect.bin`、`0x34 Dslr_Emnt_WPS_PBC.bin`、`0x35 Dslr_Emnt_WPS_Push.bin`）與 `--apply` 命令。
  - [`camera/native_film_ui_trial3.c`](camera/native_film_ui_trial3.c) 與 [`host-tests/build_ui_trial3.sh`](host-tests/build_ui_trial3.sh) 支援單槽與多槽（`A6400_CHARO_MULTI=1`）循環切換：`日文 Charo` → `Bling Bling`（或 `SD自訂`）→ `iPhone 快門` → `關閉`。
  - [`camera/film_native_command.c`](camera/film_native_command.c) 新增只讀 `IFSYS sound` 命令讀取 SD 卡 `A:/A6400/CHARO.PCM`／`SHUTTER.PCM`（`1–29,760 B`）原地覆寫 `tmpfs`；**明確規範音效必須編碼為 Sony 24 kHz SDS 32-byte ADPCM**（含對應驗證結果：43–50 dB SNR、`bad_hdr=0`）。
- **選單即時狀態顯示（`a6400_ui_status_v1`）**：
  - 在維持 `sizeof(ui) == 1120` 快照 ABI 不變前提下，新增獨立導出結構 `a6400_ui_status_v1`（magic `0x53544156`），於主標題列即時顯示轉檔中／已轉張數與 `/1024` 佇列容量，並於「膠片細節」分頁底部顯示 `佇列:M/1024 | 畫質`。
- **防截斷安全部署器、`--rollback` 與 `--diag`**：
  - [`tools/pmca_deploy_v3_clite.py`](tools/pmca_deploy_v3_clite.py) 將所有 `serviceshell` 指令分塊控制在 `<= 180` 字元並內建 `rc=None` 自動重試、支援 `extra_persistent_files` 與雙向 SHA-256 讀回校驗，新增 `--rollback`（還原 `.pre-v3c-backup` 或清除掛勾）與 `--diag`（蒐集 10 項機身狀態與日誌）。

### 效能最佳化（Performance）
- **無 NEON 環境下之位元等價 6MP 解碼倍速優化**：
  - [`vendor/picojpeg.c`](vendor/picojpeg.c) 實作 32-bit MSB 位元緩衝、暫存器快取 AC 解碼與 DC-only EOB 快速通道；[`camera/rt.c`](camera/rt.c) 改為 32-bit 字組 `memset`。
  - 主機基準測試 `A6409675-original.JPG`（`6MP`）：`decode_ms` 由 `249 ms → 138 ms`（全流程 `304 ms → 194 ms`），輸出 JPEG 與 V3.5.1 **100% 位元一致**。
  - 實機冷開機實拍（`A6409868.JPG`，`portra_400`, `grain=2`, `halation=1`, `1.7MP`）：`read=1350ms`、`render=3140ms`（`decode_ms=1166`, `film_ms=496`, `encode_ms=1147`）、`commit=1240ms`，總耗時 **`total_ms=6690`**（`result=0`、`target_verified=1`）。

### 問題修復（Fixed）
- **修復 PMCA 切換服務模式逾時（`USBTimeoutError` / `054C:0CA8 → 054C:0336` 卡死）**：
  - [`camera/film_capture_daemon.c`](camera/film_capture_daemon.c) 在 `FILM_CAPTURE_PERSISTENT` 模式下於 `scan == -2`（`media_unavailable`，含 USB 大容量儲存卸載 SD 卡時）主動呼叫 `transport_stop(&transport)` 釋放 `/usr/bin/mpr_monio.elf` 並休眠 `2000u` ms；實機冷開機後直接插線跑 `--diag` **無須 `/disable_hook` 即一次順利切入 `054C:0336`**。
- **修復 `/etc` 與 `/tmp` 共用 `/dev/nflasha10` Inode 導致誤刪開機掛勾**：
  - A6400 上 `/etc` 與 `/tmp` 同為 `/dev/nflasha10` 目錄別名（同一 inode）；[`tools/pmca_deploy_v3_clite.py`](tools/pmca_deploy_v3_clite.py) 移除在安裝 `/etc/a6400-autoexec.so` 後誤刪 `/tmp/a6400-autoexec.so` 的邏輯並保護回滾路徑。
- **修復 `/tmp_bt` 預設 `1024k tmpfs` 上限空間不足**：
  - 部署暫存改掛載獨立 `size=2048k tmpfs` 於 `/tmp_bt/v3c_up`，部署完成前自動卸載清理。
- **修正守護行程掃描與錯誤恢復**：
  - [`host-tests/build_film_capture_daemon.sh`](host-tests/build_film_capture_daemon.sh) 調整穩定窗口 `CAPTURE_WAIT_MS=4000u`、`CAPTURE_BASELINE_INTERVAL_MS=750u`、`CAPTURE_STABLE_MS=800u`。
  - [`camera/native_capture_probe.c`](camera/native_capture_probe.c) 安全略過非錯誤類 `[AVIMMGR]`／`[MprCnv]` 背景雜訊；[`camera/film_capture_daemon.c`](camera/film_capture_daemon.c) 修正 `RAM_JOB_READ_RETRY` 後誤殘留 `uncertain=1` 導致守護行程提前停止的問題。
- **修正 Linux GCC / glibc 單元測試相容性**：
  - [`test_native_film_view.py`](test_native_film_view.py) 以 `model_syscall` 避開 ELF PLT 攔截並修正 `snprintf` 重疊緩衝未定義行為，確保在 macOS 與 Ubuntu Linux（GitHub Actions CI）皆能全數通過。
- **文件與排雷規範完整化**：
  - 擴充 [`docs/部署與操作規範.md`](docs/部署與操作規範.md) 第六節「完整實戰排雷手冊」，正式改推 **Windows 10/11 使用 WSL2 + `usbipd-win --auto-attach` 部署**（避開 Win11 原生 `libusb-win32`/Zadig 驅動問題），並補齊 `--pmca-dir`、`--install-hook`、`/disable_hook`/`/A6400/DISABLE`、24 kHz Sony SDS 格式要求、`/dev/nflasha10` 空間檢查與冷開機驗收步驟。

---

## [V3.5.1 Fast SP] - 2026-11-19

> 實機驗收基準：`package=5b079b295c2a7a27`；對應公開發布：`v3.5.1-fast-sp`

- **「膠片細節」子分頁（`FILM_MENU_DETAILS`）**：主選單新增第 3 列，可於拍攝期動態切換 **顆粒強度 `0–3`**（`無 (加速)`／`輕微`／`標準`／`濃厚`）與 **底片光暈 `開 / 關`**（關光暈時略過 `film_mask_blur`，`mask_kib=0`）及一鍵恢復預設。
- **編譯與演算法提速**：[`vendor/jpge.cpp`](vendor/jpge.cpp) 啟用 `-O3`、精簡 `DCT_MUL`、移除子閾值 64-bit 乘法、`inline put_bits`／`noinline flush_output_buffer`；[`camera/author_film.c`](camera/author_film.c) 預縮放 `bell_scaled[256]` 並將水平模糊改為 `O(1)` 滑動視窗；[`camera/film_ram_job.sh`](camera/film_ram_job.sh) 以 `busybox ls -ln` 單次 `lstat` 取代 `wc -c`。
- **公開建置與架構澄清**：將 `1.7MP / 6MP` 畫質切換自私人音效宏解耦、確認實機 `/proc/cpuinfo`（ARMv7 Cortex-A5 **無 NEON**）、補足 exFAT／FAT32 原生相容性、`1,024` 張佇列上限與連拍寫尾 `stage=source_size` 安全保護實證。
- **實機實拍耗時**：`1.7MP 極速` **`5,650 ~ 5,950 ms`**（`grain=0, halation=0`）；`6MP 高清` **`17,490 ms`**。

---

## [V3.5 Fast] - 2026-11-18

> 實機基準：`package=ff85fe8a9a317b68`

- 引入 `picojpeg` 8-bit Huffman 查表與 `6MP`（`3000×2000`）真半尺度 IDCT（跳過完整 8×8 逆變換）。
- 實機 `6MP` 由 `33.41 s` 大幅縮短至 `20,960 ms`，`1.7MP` 約 `8,160 ms`（預設 `grain=2, halation=1`）。

---

## [V3.4 Base LTR] - 2026-11-18

> 實機基準：`package=2bdd6b742789b8e7`

- 主選單左下角新增 **`畫質: 1.7MP 極速 / 6MP 高清`** 一鍵切換。
- 輸出檔名改為與原圖後 4 碼直觀對應之 `F<原圖4碼><3碼亂數>.JPG`。
- 新增完整機上渲染分段計時輸出 `RAM_JOB_TIMING`。

---

## [V3.2 Universal / Charo] - 2026-11-17

- 導入執行期動態抽取機身 `/usr/lib/libAK8975.so` 產出獨立掛勾（不帶走機身專有二進位）、動態掃描 `im.elf` PID（[`camera/im_pid_find.h`](camera/im_pid_find.h)），並支援選配私人快門音效掛載測試。

---

## [V3.1 CLite] - 2026-11-17 ~ 2026-11-18

> 歷史回滾基準：`package=77e49f2132ca0878`；對應公開發布：`v3.1-clite-toast-stable-20261118`

- 建立半透明深色浮層卡片與 Sony Alpha 橘條 UI 視覺定版、左下角 Toast 進度 HUD（`a6400_ui_toast_v1`）、`-O2` 編譯與精簡寫卡雜湊驗證，確立雙向 `<= 196,608 B` 純 RAM 開機包規範。

---

## [V0 / V1 / V2 Lite] - 2026-11-14 ~ 2026-11-16

- **V2 Lite**：確定純 RAM（`/tmp_bt`）掛載架構、原生 `mwf_monio` `IFSYS` 只讀原片＋非破壞寫入 `/A6400/TEST/`、以及獨立寫卡安全驗證與鎖定機制。
- **V0 / V1 Beta**：完成 `MENU + 放大鏡` 組合鍵召喚原型、`Appframework` UI 執行緒接駁與觸控回饋測試。
