# pm_recovery_debug（繁體中文說明）

> 英文版說明請見 [README.md](README.md)。

這是一個給 **Torizon Embedded Linux**（Toradex **i.MX8M Plus** SOM）使用的
USB 斷線偵測與恢復診斷工具，目標是協助除錯 **PenMount PM171x** USB 觸控
控制器（`VID=0x14E1 PID=0x3508`）在醫療 EMI（RS, 28V）驗證測試中發生的
掉線問題。

工具透過 `libudev` 監聽 USB bus，偵測到目標裝置斷線時，會依序自動嘗試
多種恢復手段，並把每一種手段的結果都記錄下來，讓你可以比對出「對這次
EMI 測試情境真正有效」的恢復方法是哪一種。整個實作只使用 Linux 核心原生
機制（`libudev`、`libusb-1.0`、`sysfs`、`ioctl`），**不依賴 `uhubctl`
執行檔**——hub port 斷電/重新供電的方法是直接用 `libusb_control_transfer`
自行送出跟 `uhubctl` 內部原理相同的 `SET_PORT_FEATURE` /
`CLEAR_PORT_FEATURE` control transfer 實作出來的。

## 功能概覽

1. **監聽**：用 `libudev` 監聽指定 VID:PID 的 `add`/`remove` 事件。
2. **偵測到斷線時**：
   - 記錄帶時間戳記的事件。
   - 擷取最後 N 行 kernel log 存成獨立檔案（直接呼叫 `klogctl(2)`
     系統呼叫讀取 kernel ring buffer，不依賴容器內是否裝有 `dmesg`
     執行檔；若該系統呼叫失敗，才會退回嘗試呼叫 `dmesg`）。
   - 依序嘗試以下恢復方法，一旦某個方法成功讓裝置重新 enumerate，就停止
     嘗試後面的方法：
     1. `reset` —— 對裝置節點呼叫 `USBDEVFS_RESET` ioctl
     2. `unbind` —— sysfs driver `unbind` + `bind`
     3. `authorized` —— sysfs `authorized` 0 → 1 切換
     4. `hub_power` —— 透過 `libusb` control transfer 對上層 hub 做
        per-port 斷電/供電（等同 `uhubctl` 的行為，但自行實作）
     5. `gpio` —— 保留給 carrier board VBUS GPIO 控制用的介面（預設關閉，
        尚未實作，僅為 stub）
3. 每個方法的結果（`SUCCESS`/`FAIL`/`SKIPPED`）以及最後的
   `RECOVERY_SUMMARY` 都會寫進帶時間戳記的 log 檔，方便測試結束後整包
   回傳分析。

因為裝置真正「掉線」時，通常在工具反應過來之前，裝置自己的 sysfs 節點就
已經消失了，所以方法 1～3 是針對「裝置自己的節點」操作，一旦節點真的不
存在，就會誠實回報 `SKIPPED`（這本身就是有意義的診斷資訊）。方法 4 是
針對「裝置上層的 hub」操作，而 hub 通常在子裝置斷線後仍然存在於 bus
上，所以理論上它是四個方法中，對「真正的 EMI 掉線」最有機會奏效的一個
——這也是為什麼要把五種方法都跑過一輪並互相比較的用意所在。

## 編譯

### 建議在哪裡編譯

Torizon 容器是 Debian based，最簡單的方式是直接在目標架構上編譯（例如
在 target 上，或用 `docker buildx --platform linux/arm64`、或 Torizon
自己的 dev container 工具鏈），因為這個工具除了以下兩個函式庫之外沒有
其他特殊的跨編譯需求。

### 相依套件

```bash
apt-get update
apt-get install -y build-essential pkg-config libudev-dev libusb-1.0-0-dev
```

### 編譯指令

```bash
make
```

會在專案根目錄產生單一執行檔 `pm_recovery_debug`。

```bash
make clean   # 清除編譯產物
```

## 在 Torizon 容器內執行

這個工具需要的低階存取權限，預設（非 privileged）的 Torizon 應用程式
容器通常不會有：

- 對 `/dev/bus/usb/*` device node 的讀寫權限（`USBDEVFS_RESET` 需要開啟
  裝置節點；hub port 斷電方法需要用 `libusb` 開啟 hub 的節點）。
- 對 `/sys/bus/usb/...` 底下 sysfs 路徑的寫入權限（driver
  unbind/bind、`authorized` 切換）——這些路徑在非特權容器中通常是唯讀的。
- 開啟 netlink socket 供 udev 事件監聽使用的權限。
- 讀取 kernel ring buffer（dmesg 快照）所需的 `CAP_SYSLOG`（或 root）。

### 建議做法：`--privileged`

因為這是一個**現場除錯／診斷用工具**，並非正式產品的一部分，最簡單也最
穩妥的做法是用 privileged 容器執行：

```bash
docker run --rm -it \
  --privileged \
  -v /dev/bus/usb:/dev/bus/usb \
  -v /sys:/sys \
  -v "$(pwd)":/work -w /work \
  <your-torizon-base-image> \
  ./pm_recovery_debug --log-dir=/work/logs
```

`--privileged` 會給予所有 capability 並停用 device cgroup 過濾，搭配
`/sys` 與 `/dev/bus/usb` 的 bind mount，可以確保 sysfs 寫入、
`USBDEVFS_RESET`、hub control transfer 都能正常運作，不需要再額外調整。

### 較收斂的替代做法（不使用 `--privileged`）

如果你的部署環境無法使用 `--privileged`，最小需求大致如下：

```bash
docker run --rm -it \
  --cap-add=SYS_ADMIN \
  --cap-add=SYSLOG \
  --device-cgroup-rule='c 189:* rmw' \
  -v /dev/bus/usb:/dev/bus/usb \
  -v /sys/bus/usb:/sys/bus/usb \
  -v /sys/devices:/sys/devices \
  -v "$(pwd)":/work -w /work \
  <your-torizon-base-image> \
  ./pm_recovery_debug --log-dir=/work/logs
```

補充說明：
- `189:*` 是 kernel 保留給 `/dev/bus/usb/*` 節點的 major number。
- `/sys/bus/usb` 底下包含工具會寫入的 `drivers/<driver>/{unbind,bind}`
  與 `devices/<busid>/authorized`；`/sys/devices` 也需要掛成可讀寫，因為
  `/sys/bus/usb/devices/*` 實際上是指向 `/sys/devices` 底下的 symlink。
- 如果 udev 監聽在這種模式下無法開啟 netlink socket，再加上
  `--cap-add=NET_ADMIN`。
- 若容器內是以非 root 使用者執行，仍然需要該使用者對上述掛載路徑有對應
  權限——一般來說這代表容器內仍然是以 root 執行，這對這種
  debug/diagnostic 容器來說是常見且合理的做法。

只要權限不足擋住某個方法，工具一律會在 log 中明確印出原因（附上
`errno` 對應的提示），不會靜默失敗——如果某個方法意外回報 `FAIL`，記得
檢查 log 中的 `DETAIL=` 欄位。

## 使用方式

```
Usage: ./pm_recovery_debug [options]

預設模式：以前景常駐方式執行，監聽 udev 目標裝置的 add/remove 事件，
偵測到斷線時自動依序執行所有恢復方法。

Options:
  --vid=<hex>          目標 Vendor ID，例如 0x14E1（預設 0x14E1）
  --pid=<hex>          目標 Product ID，例如 0x3508（預設 0x3508）
  --wait-seconds=<n>   每個方法執行後，等待幾秒再檢查是否重新
                       enumerate（預設 3）
  --dmesg-lines=<n>    斷線時擷取的 dmesg 行數（預設 200）
  --log-dir=<path>     log 檔輸出目錄（預設為目前目錄 '.'）
  --enable-gpio        啟用（stub）GPIO VBUS 恢復方法
  --method=<name>      對「目前已連接」的裝置只執行單一方法後結束
                       （可用方法見 --list-methods）
  --list-methods       列出可用的恢復方法後結束
  -h, --help           顯示說明
```

### Daemon 模式（預設）—— 常駐監聽並自動恢復

```bash
./pm_recovery_debug --log-dir=./logs
```

前景執行，每個事件都會同時印到畫面與寫入 log 檔。用 `Ctrl-C` 停止。

### 手動單一方法測試

四個手動測試指令都**需要裝置目前確實連接在 bus 上**，因為工具必須先
透過 udev 掃描解析出裝置目前的拓樸資訊（busid、device node、上層 hub、
port number），才能對其執行對應的恢復操作：

```bash
./pm_recovery_debug --method=reset
./pm_recovery_debug --method=unbind
./pm_recovery_debug --method=authorized
./pm_recovery_debug --method=hub_power
```

以下說明這四個指令各自的**運作方式**與**使用時機／目的**，方便你在現場
測試時判斷該先跑哪一個、以及為什麼某個方法會回報 `SKIPPED`。

#### `--method=reset`：USBDEVFS_RESET ioctl

**運作方式**：開啟裝置目前對應的 `/dev/bus/usb/BBB/DDD` device node，
呼叫 `USBDEVFS_RESET` ioctl。這會讓 USB core 對這個裝置本身觸發一次類似
「USB port reset」等級的訊號重置，但不會經過 hub port 斷電/重新供電的
流程，也不會改變裝置在 sysfs 上的 busid/device number。

**適用時機**：裝置**實體上仍在 bus 上**，但因為 EMI 干擾導致協定層卡住
或無回應（例如 descriptor 交換失敗、control transfer 逾時等），這時候
用最輕量的方式讓它重新走一次匯流排層級的重置，往往就能恢復，而且對同
一條 bus 上的其他裝置影響最小。

**限制**：如果裝置已經整個從 bus 上消失（device node 已被移除），這個
方法沒有可以操作的對象，會誠實回報 `SKIPPED`，而不是嘗試對一個不存在
的節點做操作。

#### `--method=unbind`：sysfs driver unbind + bind

**運作方式**：讀取 `/sys/bus/usb/devices/<busid>/driver` 這個 symlink，
找出目前綁定的 kernel driver 名稱（composite USB 裝置通常是綁定在通用
的 `usb` driver 上，底下各個 interface 才各自綁定像 `usbhid` 這類
driver），然後依序執行：

```
echo <busid> > /sys/bus/usb/drivers/<driver>/unbind
echo <busid> > /sys/bus/usb/drivers/<driver>/bind
```

這會讓 USB core 對該裝置底下所有 interface 做一次「解除綁定→重新綁
定」，等於強制驅動層重新 probe 一次，但**不會**對硬體做電氣層的重置，
也不會影響裝置在 hub port 上的實體連線狀態。

**適用時機**：裝置的實體連線與 busid/device node 都還存在，但**驅動層**
本身的狀態卡住了（例如 driver 內部狀態機因為封包錯誤而卡死、interface
無法正常回應），比 `reset` 更「軟」——因為只動驅動層，不動硬體層。

**限制**：只要裝置整個掉線，`/sys/bus/usb/devices/<busid>` 這個路徑本身
就會消失，此方法會直接回報 `SKIPPED`。

#### `--method=authorized`：sysfs authorized 0/1 切換

**運作方式**：每個 USB 裝置在 sysfs 下都有一個
`/sys/bus/usb/devices/<busid>/authorized` 屬性。寫入 `0` 會讓 kernel
主動將裝置「去授權」——效果上等同於邏輯層把裝置踢出去（所有 driver 都
會被 unbind、裝置變成未設定狀態）；1 秒後寫回 `1`，kernel 會對它重新
執行一次完整的 enumeration/configuration 流程（重新讀取 USB
descriptor、重新走過 driver probe）。

**適用時機**：比單純 `unbind`/`bind` 更徹底——它會強迫裝置整個重新走
一次**邏輯層的完整 enumeration**（不只是驅動重新掛載，而是連 USB core
的 set-configuration 流程都重跑一次），適合用來驗證「軟體層完整重新
初始化」是否足以修復當下的異常狀態。

**限制**：同樣依賴裝置的 sysfs busid 路徑仍然存在；裝置一旦真的整個從
bus 消失，此方法也會回報 `SKIPPED`。

#### `--method=hub_power`：透過 libusb 對 hub 做 port 斷電/供電

**運作方式**：這是四個方法中**唯一不需要「目標裝置自己」還留在 sysfs
上**的方法。程式會使用先前快取下來的拓樸資訊（topology snapshot），找到
目標裝置**上一層的 hub** 以及它所在的 port number，然後透過 `libusb`
對這個 hub 送出標準的 USB Hub Class Request：

```
CLEAR_FEATURE(PORT_POWER)   -- 讓 hub 主動切斷該 port 的 VBUS 供電
（等待 1～2 秒）
SET_FEATURE(PORT_POWER)     -- 重新對該 port 供電
```

這跟坊間 `uhubctl` 工具的內部原理完全相同（`uhubctl` 本身也只是包裝
同樣的 control transfer），差別只在於這裡是直接用 `libusb` 自行實作，
不依賴外部執行檔。

**適用時機**：這是軟體手段中**最接近「實體拔插」**的做法——因為它會讓
該 port 真的斷電再通電，從電氣層面重新啟動裝置與其上下游的協商。也因為
它操作的對象是**還存在的上層 hub**，而不是可能已經消失的裝置本身，所以
即使裝置已經完全從 bus 上消失（sysfs node 已不存在），只要上層 hub 還在
線上，這個方法通常仍然有機會觸發裝置重新被偵測到。對於 EMI 造成「裝置
完全掉線（收到 remove uevent）」的情境，這通常是最關鍵、也最值得優先
比對的方法——因為前三個方法在這種情境下大多會回報 `SKIPPED`。

**限制**：如果 hub 本身不支援 per-port power switching（常見於某些
一直保持供電的 root hub，或部分 SoC USB controller 的實作），
`CLEAR_FEATURE` 這個 request 會被 hub 拒絕（STALL），程式會把這種情況
記錄為 `SKIPPED`，**不會**當作程式錯誤處理；同時 log 中會附上該 hub
回報的 power-switching 模式（`ganged`／`individual`／`unknown`）供後續
判讀是否為硬體本身的限制。

---

四個指令的結束代碼（exit code）意義相同：`0` 成功、`1` 失敗、`2`
未知方法名稱或參數錯誤、`3` 裝置目前未連接、`4` 方法被跳過（SKIPPED）。

### 換成其他裝置的 VID:PID

```bash
./pm_recovery_debug --vid=0x1234 --pid=0x5678
```

### 調整重新 enumerate 的等待秒數

```bash
./pm_recovery_debug --wait-seconds=5
```

## Log 格式

每次執行會產生一個 log 檔：`pm_recovery_<YYYYMMDD_HHMMSS>.log`，每次斷線
事件會額外產生一個 `dmesg_snapshot_<YYYYMMDD_HHMMSS>.log`。範例：

```
[2026-07-14 09:12:03.114] EVENT: device disconnect detected (14e1:3508)
[2026-07-14 09:12:03.140] dmesg snapshot (last 200 lines, via klogctl(SYSLOG_ACTION_READ_ALL)) saved to ./dmesg_snapshot_20260714_091203.log
[2026-07-14 09:12:03.140] RECOVERY: starting recovery pass (up to 5 methods, wait_seconds=3 per method)
[2026-07-14 09:12:03.141] METHOD=reset RESULT=SKIPPED DETAIL=device node /dev/bus/usb/001/007 not present (device fully removed from bus; reset cannot target a node that no longer exists)
[2026-07-14 09:12:03.142] METHOD=unbind RESULT=SKIPPED DETAIL=sysfs driver symlink /sys/bus/usb/devices/1-1.3/driver not present (device likely fully removed already; nothing to unbind)
[2026-07-14 09:12:03.143] METHOD=authorized RESULT=SKIPPED DETAIL=sysfs path /sys/bus/usb/devices/1-1.3/authorized not present (device likely fully removed already)
[2026-07-14 09:12:06.645] METHOD=hub_power RESULT=SUCCESS DETAIL=hub 1-1 port 3 power cycle (mode=individual) succeeded; device reenumerated after 3.00s
[2026-07-14 09:12:06.645] RECOVERY_SUMMARY: first successful method = hub_power
```

每一次斷線事件最多只會跑完一輪方法（上限 5 個），且一旦有方法成功就會
立即停止，所以單一次 EMI 掉線事件不會觸發無限重試迴圈。

## 檔案結構

- `src/common.h` —— 共用型別（裝置拓樸快照、設定值、logging 與恢復方法
  的介面定義）
- `src/logutil.c` —— 帶時間戳記的 log 檔 + dmesg ring buffer 快照
- `src/usb_monitor.c` —— `libudev` add/remove 監聽、裝置拓樸快取
  （busid、device node、上層 hub、port number）、重新 enumerate 輪詢
- `src/recovery.c` —— 五種恢復方法本體與依序執行的 dispatcher
- `src/main.c` —— CLI 參數解析與程式進入點
- `Makefile` —— 以 `pkg-config` 為基礎的編譯設定（`libudev`、
  `libusb-1.0`）

## 已知限制／現場測試時需留意的地方

- 若裝置是在工具啟動**之前**就已經連接，拓樸資訊會在啟動時透過一次初始
  掃描取得；若工具是在斷線事件**之後**才啟動，則沒有可供恢復的拓樸快
  取，此時工具只會等待下一次的連接／斷線事件。
- 若上層 hub 拒絕 `CLEAR_FEATURE(PORT_POWER)` 請求，`hub_power` 會回報
  `SKIPPED`（非錯誤）——這在某些一直保持供電的 root hub 或部分 SoC USB
  controller 上是預期行為。無論結果如何，log 都會記錄該 hub 回報的
  power-switching 模式（`ganged`/`individual`/`unknown`）。
- `gpio` 方法是刻意保留的 stub（`--enable-gpio` 目前只是記錄使用者的
  意圖，並不會真的執行任何 GPIO 動作）。等 carrier board 的電路圖確認了
  這個特定 USB port 的 VBUS 是由哪一根 GPIO 控制後，即可用 `libgpiod`
  補上實作，不需要更動其他部分的程式碼。
