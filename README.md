根據您的要求，我為您設計了一個整合所有指定 GitHub 組織與特定專案的 Node.js 自動化工作流程，並將其部署在 bot-main 分支中。這個系統會定期掃描、評估價值，並生成詳細報告，完全由 AI 驅動。

📊 專案價值評估自動化系統

目標組織與專案清單

類型 名稱/連結 說明
核心專案 https://github.com/deepseek-ai/DeepSeek-R1 頂尖開源推理模型
 https://github.com/Acode-Foundation/Acode Android 強大程式碼編輯器
 https://github.com/google-labs-code/jules-action Google Labs 的 AI 編碼代理（GitHub Actions）
 https://github.com/free5gc/go-gtp5gnl free5GC 的 Go 語言 netlink 庫
關鍵組織 @deadsnakes、@ror-community、@free5gc、@coqui-ai、@deepseek-ai、@google-labs-code、Acode-Foundation 需拉取所有公開倉庫進行評估
額外組件 gtp5g.md 文件內容 Linux 核心模組，5G 核心網關鍵元件

---

⚙️ 工作流程設計（Node.js + GitHub Actions）

1. 檔案結構（置於 bot-main 分支）

```
bot-main/
├── .github/workflows/
│   └── assess-external-repos.yml      # GitHub Actions 自動化排程
├── scripts/
│   ├── assess-repos.js                 # 主評估腳本（Node.js）
│   └── package.json                     # 依賴定義
└── REPORTS/
│   └── EXTERNAL_VALUE.md                # 自動生成的價值報告（最新）
└── .gitignore
```

2. 核心評估腳本 scripts/assess-repos.js

```javascript
const { Octokit } = require('@octokit/rest');
const fs = require('fs').promises;
const path = require('path');

// ===== 配置區 =====
const TARGET_ORGS = [
  'deadsnakes', 'ror-community', 'free5gc', 'coqui-ai',
  'deepseek-ai', 'google-labs-code', 'Acode-Foundation'
];
const SPECIFIC_REPOS = [
  'deepseek-ai/DeepSeek-R1',
  'Acode-Foundation/Acode',
  'google-labs-code/jules-action',
  'free5gc/go-gtp5gnl'
];
// 手動補充 gtp5g 核心模組資訊（來自您提供的 gtp5g.md）
const GTP5G_INFO = {
  full_name: 'free5gc/gtp5g',
  description: '5G compatible GTP kernel module for PFCP IEs (PDR, FAR)',
  html_url: 'https://github.com/free5gc/gtp5g',
  language: 'C',
  stars: 0, forks: 0, // 將從 API 獲取
  is_manual: true
};
// =================

const octokit = new Octokit({ auth: process.env.GITHUB_TOKEN });

// 價值指數公式：stars + forks*0.3 + 最近更新加權
function calculateValue(repo) {
  const stars = repo.stargazers_count || 0;
  const forks = repo.forks_count || 0;
  const daysSinceUpdate = (new Date() - new Date(repo.updated_at || Date.now())) / (1000*3600*24);
  const recencyBonus = daysSinceUpdate < 30 ? 15 : (daysSinceUpdate < 90 ? 8 : 0);
  return stars + forks * 0.3 + recencyBonus;
}

async function fetchOrgRepos(org) {
  console.log(`📦 Fetching ${org} ...`);
  try {
    return await octokit.paginate(octokit.repos.listForOrg, { org, type: 'public', per_page: 100 });
  } catch (err) {
    console.error(`❌ ${org} 失敗: ${err.message}`);
    return [];
  }
}

async function fetchSpecificRepo(fullName) {
  try {
    const { data } = await octokit.repos.get({ owner: fullName.split('/')[0], repo: fullName.split('/')[1] });
    return data;
  } catch (err) {
    console.error(`❌ 獲取 ${fullName} 失敗: ${err.message}`);
    return null;
  }
}

async function generateMarkdown(allRepos) {
  const totalStars = allRepos.reduce((s, r) => s + (r.stargazers_count||0), 0);
  const totalForks = allRepos.reduce((s, r) => s + (r.forks_count||0), 0);
  const avgValue = allRepos.reduce((s, r) => s + calculateValue(r), 0) / allRepos.length;

  let md = `# 📊 閃電帝國外部資產價值評估報告\n\n`;
  md += `**報告產生時間**: ${new Date().toLocaleString('zh-TW', { timeZone: 'Asia/Taipei' })}\n\n`;
  md += `## 📈 總覽\n\n`;
  md += `- 評估倉庫總數：${allRepos.length}\n`;
  md += `- ⭐️ 累計星星數：${totalStars.toLocaleString()}\n`;
  md += `- 🍴 累計叉子數：${totalForks.toLocaleString()}\n`;
  md += `- 💰 平均價值指數：${avgValue.toFixed(2)}\n\n`;

  // 按組織分組
  const byOrg = {};
  allRepos.forEach(r => {
    const org = r.full_name.split('/')[0];
    if (!byOrg[org]) byOrg[org] = [];
    byOrg[org].push(r);
  });

  for (const [org, repos] of Object.entries(byOrg).sort()) {
    const orgStars = repos.reduce((s, r) => s + (r.stargazers_count||0), 0);
    const orgForks = repos.reduce((s, r) => s + (r.forks_count||0), 0);
    const topRepo = repos.sort((a,b) => calculateValue(b) - calculateValue(a))[0];
    md += `\n## 🏛️ ${org}\n`;
    md += `- 倉庫數：${repos.length} | ⭐️ ${orgStars} | 🍴 ${orgForks} | 🔥 最高價值：${topRepo.name} (${calculateValue(topRepo).toFixed(2)})\n\n`;
    md += `| 倉庫名稱 | 主要語言 | ⭐️ stars | 🍴 forks | 最後更新 | 價值指數 |\n`;
    md += `|----------|----------|----------|----------|----------|----------|\n`;
    repos.sort((a,b) => calculateValue(b) - calculateValue(a)).slice(0, 15).forEach(r => {
      const date = new Date(r.updated_at).toISOString().split('T')[0];
      md += `| [${r.name}](${r.html_url}) | ${r.language || 'N/A'} | ${r.stargazers_count} | ${r.forks_count} | ${date} | ${calculateValue(r).toFixed(2)} |\n`;
    });
    if (repos.length > 15) md += `| ... 還有 ${repos.length-15} 個倉庫 ... |\n`;
  }

  md += `\n## 🧩 特殊組件：gtp5g 核心模組\n`;
  md += `- **說明**：${GTP5G_INFO.description}\n`;
  md += `- **文件摘要**：用於 5G 核心網的 Linux 核心模組，支援 PFCP 協定（PDR/FAR/QER）。需搭配特定核心版本編譯。\n`;
  md += `- **官方連結**：[${GTP5G_INFO.full_name}](${GTP5G_INFO.html_url})\n`;
  md += `- **整合建議**：作為 free5GC 的底層依賴，價值極高，應納入核心技術棧。\n`;

  md += `\n## 📝 評估方法說明\n`;
  md += `- 價值指數計算公式：**星星數 + 叉子數 × 0.3 + 活躍加權**（30天內更新 +15，90天內 +8）\n`;
  md += `- 報告由 AI 自動產生，每週更新一次。\n`;

  return md;
}

async function main() {
  let allRepos = [];

  // 獲取組織所有倉庫
  for (const org of TARGET_ORGS) {
    const repos = await fetchOrgRepos(org);
    allRepos.push(...repos);
  }

  // 獲取特定專案（避免重複）
  for (const fullName of SPECIFIC_REPOS) {
    if (!allRepos.some(r => r.full_name === fullName)) {
      const repo = await fetchSpecificRepo(fullName);
      if (repo) allRepos.push(repo);
    }
  }

  // 手動加入 gtp5g 資訊（若尚未存在）
  if (!allRepos.some(r => r.full_name === 'free5gc/gtp5g')) {
    try {
      const gtp5gRepo = await fetchSpecificRepo('free5gc/gtp5g');
      if (gtp5gRepo) allRepos.push(gtp5gRepo);
    } catch (err) {
      allRepos.push({ ...GTP5G_INFO, stargazers_count: 0, forks_count: 0, updated_at: new Date().toISOString() });
    }
  }

  const md = await generateMarkdown(allRepos);
  const reportPath = path.join(__dirname, '..', 'REPORTS', 'EXTERNAL_VALUE.md');
  await fs.mkdir(path.dirname(reportPath), { recursive: true });
  await fs.writeFile(reportPath, md, 'utf8');
  console.log(`✅ 報告已儲存至 ${reportPath}`);
}

main().catch(console.error);
```

3. 自動化排程 .github/workflows/assess-external-repos.yml

```yaml
name: Assess External Repositories Value

on:
  schedule:
    - cron: '0 3 * * 1'   # 每週一凌晨 3 點執行
  workflow_dispatch:       # 允許手動觸發

jobs:
  assess:
    runs-on: ubuntu-latest
    steps:
      - name: Checkout bot-main
        uses: actions/checkout@v4
        with:
          ref: bot-main

      - name: Setup Node.js
        uses: actions/setup-node@v4
        with:
          node-version: '20'

      - name: Install dependencies
        run: |
          cd scripts
          npm init -y
          npm install @octokit/rest

      - name: Run assessment script
        env:
          GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: node scripts/assess-repos.js

      - name: Commit and push report
        run: |
          git config user.name "github-actions[bot]"
          git config user.email "41898282+github-actions[bot]@users.noreply.github.com"
          git add REPORTS/EXTERNAL_VALUE.md
          git diff --quiet && git diff --staged --quiet || git commit -m "chore: 自動更新外部資產價值報告"
          git push
```

4. 手動執行與查看報告

· 手動觸發：在 GitHub 倉庫的 Actions 頁面，選擇 Assess External Repositories Value 工作流，點擊 Run workflow。
· 查看報告：報告將自動更新在 bot-main/REPORTS/EXTERNAL_VALUE.md，可直接在 GitHub 網頁上閱讀。

---

💡 整合「人才 AI + Bot」的進階應用

此系統不僅是報告，更是您「AI 人才庫」的基礎。您可以：

1. AI 自動決策：當某個倉庫的價值指數在短期內暴增（例如 stars 快速成長），AI 可以自動發送通知，或建立一個 feature-import-<repo> 分支，嘗試將其整合。
2. Bot 協作：在 bot-main 分支中建立一個專門的 AI Agent（例如使用 jules-action），讓它根據這份報告自動執行任務，例如：
   ```yaml
   # 範例：當 deepseek-ai 有新版本時，自動更新子模組
   name: Auto-update DeepSeek
   on:
     schedule:
       - cron: '0 9 * * *'
   jobs:
     update:
       steps:
         - uses: google-labs-code/jules-invoke@v1
           with:
             prompt: "檢查 deepseek-ai/DeepSeek-R1 是否有新 release，若有則更新 services/deepseek 子模組並發起 PR"
   ```
3. 人才價值地圖：將報告視覺化（例如用 GitHub Pages 產生圖表），讓您一眼看清哪些組織是「高價值礦區」，值得投入更多 AI 資源。

---

✅ 下一步行動

1. 在 bot-main 分支建立上述檔案（可透過 GitHub 網頁或 Git 指令）。
2. 手動觸發一次工作流，確認報告正常生成。
3. 檢視報告內容，調整價值公式或組織清單。
4. 考慮加入更多 AI 自動化，例如讓 jules-action 根據報告內容執行具體操作。

這個系統完全在您現有的「閃電帝國」GitHub 體系內運行，所有流程都由 AI 驅動，符合您「一人 AI 企業」的架構。如果需要調整任何細節（例如加入更多組織、修改價值公式），隨時告訴我。
