# lightning_notify.py
import requests
from datetime import datetime
import sys

# ====== 載入設定 ======
try:
    import config
except ImportError:
    print("❌ 錯誤：找不到 `config.py` 設定檔。")
    print("請將 `config.py.example` 複製為 `config.py` 並填入您的設定。")
    sys.exit(1)

# 檢查設定是否完整
if not hasattr(config, 'TELEGRAM_BOT_TOKEN') or not hasattr(config, 'CHAT_ID'):
    print("❌ 錯誤：`config.py` 中缺少 `TELEGRAM_BOT_TOKEN` 或 `CHAT_ID`。")
    sys.exit(1)

# ====== 範例資料 ======
tasks = [
    {"task_id": 1, "區域": "逢甲", "內容": "派送包裹A"},
    {"task_id": 2, "區域": "福科", "內容": "派送包裹B"}
]

# ====== 發送通知函數 ======
def notify_lightning_cat(task_list):
    """
    發送任務清單到指定的 Telegram 頻道。
    """
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    message = f"⚡️ 手動派單通知 - {timestamp}\n\n"
    for t in task_list:
        message += f"任務ID: {t['task_id']}\n區域: {t['區域']}\n內容: {t['內容']}\n\n"

    url = f"https://api.telegram.org/bot{config.TELEGRAM_BOT_TOKEN}/sendMessage"
    payload = {"chat_id": config.CHAT_ID, "text": message}

    try:
        r = requests.post(url, data=payload)
        if r.status_code == 200:
            print("✅ 派單通知已成功發送！")
        else:
            print(f"❌ 發送失敗，狀態碼：{r.status_code}, 回應：{r.text}")
    except requests.exceptions.RequestException as e:
        print(f"❌ 網路連線錯誤：{e}")

# ====== 主程式 ======
if __name__ == "__main__":
    # 檢查 TOKEN 和 CHAT_ID 是否為預設值
    if "在這裡填入" in config.TELEGRAM_BOT_TOKEN or "在這裡填入" in str(config.CHAT_ID):
        print("⚠️ 警告：請在 `config.py` 中填入您真實的 Token 和 CHAT_ID。")
    else:
        notify_lightning_cat(tasks)