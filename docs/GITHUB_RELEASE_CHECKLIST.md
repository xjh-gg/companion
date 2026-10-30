# GITHUB_RELEASE_CHECKLIST

日期：2026-10-23
结论：**可以进入私有仓库准备，公开前仍需完成人工决策与清理。**

## 1. 发布前必须确认的决策

- [ ] 仓库可见性：public / private（本轮不自动公开）。
- [ ] 源码策略：
  - 方案 A（推荐）：新建干净仓库，以 XiaoZhi 2.0.3 为基线，应用 `patches/` 五个补丁，再放入本仓库的 `main/companion`、文档、测试。
  - 方案 B：把当前工作区作为“资料/补丁仓库”，明确声明不含完整可构建固件源码。
- [ ] 是否提交早期自研原型 `main/` + `components/`；若提交，需在 README 中持续标注“legacy”。
- [ ] 固件二进制是否作为 Release 附件单独发布（不建议提交进 Git）。
- [ ] 第三方许可证与版权声明（XiaoZhi、ES8388 驱动、LVGL、cJSON 等）。

## 2. 检查结果

| 检查项 | 状态 | 说明 |
|---|---|---|
| Git 仓库 | **未初始化** | 无 `.git`、无分支/提交；未执行 `git init` |
| 未提交修改 | 不适用 | 非 Git 仓库 |
| README | PASS | 已按真实代码重写，区分已实现/部分验证/未实现 |
| `.gitignore` | PASS | 已忽略 `backups/`、build、sdkconfig、exe、缓存、证书类文件 |
| 交付补丁 | PASS | 5 个补丁已复制到 `patches/`，哈希已记录 |
| 固件/日志 | PASS（不提交） | 保留在 `backups/`，`.gitignore` 排除 |
| 敏感信息 | **PARTIAL** | `backups/` 已排除；文档路径/IP 已脱敏；Kconfig 与测试脚本仍有本机默认值 |
| 第三方许可证 | **待确认** | 当前工作区未重新授权，需保留上游 LICENSE/版权 |
| 构建可复现性 | **PARTIAL** | 补丁已齐；完整干净 XiaoZhi 树的一次性重建本轮未执行 |
| 主机测试 | PASS（有前提） | 98 项通过；脚本依赖 `-SourceRoot`，默认仍是本机临时路径 |
| 物理验收 | **PARTIAL** | LED/按键/运动通过；扬声器听音、MCP 外部调用、断网恢复未验证 |
| MQTT 云链路 | **部分实现** | 设备侧事件/状态上报与安全下行已实现；云端消费者/AI 未实现，不得写入 release notes 为端到端完成 |
| Release/推送 | 未执行 | 未创建仓库、未提交、未推送、未改可见性 |

## 3. 公开前必须处理

1. **测试脚本默认路径**
   - `tests/host/run_tests.ps1:2` 默认 `-SourceRoot` 含本机临时绝对路径。
   - 建议改为必填参数、环境变量或相对路径；本轮未改测试代码。

2. **Kconfig 默认值**
   - `main/Kconfig.projbuild` 的 SSID/密码是占位符，但 MQTT broker 默认值为本地网段地址。
   - 建议改为通用示例（如 `mqtt://192.168.1.100:1883`）或空值；不得写入真实凭据。

3. **源码策略与许可证**
   - 当前工作区不等于完整 XiaoZhi 源码；直接公开会让读者无法构建产品固件。
   - 推荐干净仓库方案 A；保留 XiaoZhi 上游 `LICENSE`、版权声明与依赖许可。

4. **文档范围**
   - `阶段1验收_小智固件兼容性报告.md` 仍在根目录；可保留，也可移入 `docs/archive/`（移动前确认无引用）。
   - 旧文档已加历史说明，不再作为当前实现依据。

5. **二进制策略**
   - 不提交 `backups/**/*.bin`、`*.log`、`*.zip`、`sdkconfig*`。
   - 如需发布固件，使用 GitHub Release 附件，并附 SHA-256 与烧录说明。

## 4. 建议的 Git 发布步骤

```powershell
# 1. 在确认源码策略后，于干净目录初始化
git init
git add README.md .gitignore docs tests patches tools CMakeLists.txt main components
# 2. 审核暂存内容（不要 add backups/ 或 sdkconfig）
git status
git diff --cached --stat
# 3. 再做一次敏感信息扫描（凭据、Token、私钥、个人路径）
# 4. 本地提交
git commit -m "docs: finalize companion project documentation and delivery patches"
# 5. 创建私有远程并推送（需用户确认）
# 6. 在干净 XiaoZhi 2.0.3 + patches 环境执行一次构建与 98 项主机测试
# 7. 确认 README/许可证/Release 说明后，再考虑公开
```

## 5. 禁止事项

- 不提交真实 Wi-Fi/MQTT 凭据、Token、私钥、证书。
- 不提交 `backups/`、固件、串口日志、sdkconfig。
- 不修改第三方许可证。
- 不自动创建公开仓库、推送、改可见性或发布 Release。
- 不把“代码存在”写成“实机已通过”。

## 6. 发布状态判定

- **私有仓库准备：READY**（文档与补丁已整理，敏感目录已排除）。
- **公开仓库：NOT READY**（源码策略、许可证、测试脚本路径、Kconfig 默认值、物理端到端验收仍需处理）。
