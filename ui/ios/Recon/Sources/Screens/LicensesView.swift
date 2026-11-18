//
//  LicensesView.swift
//  Recon — U-13 开源许可清单页（iOS）
//
//  契约：docs/09_UI规格.md §3.13（元素表、线框图、U-L-1..4）、§2（`licenses` 路由）、
//       §5.9（`msg.licenses.title` / `msg.licenses.intro` 两个键）、
//       docs/12_构建与打包.md §2.7（许可清单生成与嵌入位置）、§2.1 D-3（vendor 要求）。
//
//  ── 本页为什么必须存在（登记说明，v1.3 补）──────────────────────────────
//  v1.2 及以前本页**无编号**，却被 §3.12 的「开源许可」行与 LG-4 的许可说明
//  引用为「推入许可清单页（`licenses` 路由）」—— 又一个「下游引用已存在、
//  上游定义缺失」，与 U-11 / U-12 同型。
//  目的：履行 MIT/Apache-2.0/BSD 系依赖的**署名义务**，纯文本、零网络。
//
//  ── 数据来源（U-L-1：不手工维护副本）──────────────────────────────────
//  内容**全部**读包内的 `THIRD_PARTY_LICENSES.txt`，该文件由
//  `cmake/Licenses.cmake` 从 `cmake/ThirdParty.cmake`（**唯一真源**）生成
//  （依赖名 / 许可 / upstream / ref 全从它派生，本页与 Swift 侧**零重复登记**）。
//
//  ⚠️ **路径必须用包根**：iOS 是**扁平** bundle，非本地化资源在包根
//  （`Recon.app/THIRD_PARTY_LICENSES.txt`），**不存在** `Contents/` 一层
//  —— 这正是闸门 **G15-c** 看管的事实（`cmake/Licenses.cmake` 的嵌入
//  `DESTINATION` 已修正为包根）。若本页写成 `.../Contents/Resources/...`，
//  在真机上会**永远读不到**（且编译期全绿 → 静默空列表，最难查的一类）。
//
//  ── 本页不渲染许可全文（与 §3.13 线框图一致）──────────────────────────
//  线框图与元素表只给出「组件名 + 许可名 + 版权行」三行/条目，**没有**全文展示。
//  全文仍随包（U-L-3 的合规要求），但本页不展开 —— 不擅自新增线框未定义的 UI。
//
//  ── 「尚未 vendor 的依赖」区（规格未定义，本条为如实呈现）──────────────
//  清单文件末尾（当有依赖未 vendor 时）会有一段
//      `===== 尚未 vendor 的依赖（清单暂缺，见 docs/12 §2.1 D-3）=====`
//  §3.13 的元素表**未定义**这一段如何呈现（它既不是依赖条目，无 SPDX 名与版权行）。
//  本实现选择**如实呈现**（U-D-7 不隐藏降级）：以 `InlineNotice(.info)` 显示该段
//  原文，使「本清单尚不完整」这一事实对用户可见，而不是静默只列已 vendor 的几条。
//  **待裁决**：是否要为该区单列呈现规则与文案键（当前直接取文件原文，
//  因它是**内容本身**而非 UI 文案，无需 locale 键）。
//
//  ── 依赖未 vendor 的现状（真实数据，非本页缺陷）────────────────────────
//  本机实测 `build_cmake/THIRD_PARTY_LICENSES.txt`：12 项依赖中**仅 2 项**
//  （`poisson_recon` / `xatlas`）有许可全文，其余 10 项落入「尚未 vendor」区。
//  即当前清单在**合规上是快照不完整**的 —— 依 docs/12 §2.1 D-3，**发布前必须
//  补齐**。本页如实呈现，不掩盖。
//

import SwiftUI

public struct LicensesView: View {

    public init() {}

    // =======================================================================
    // MARK: - 解析结果模型
    // =======================================================================

    /// 一个依赖条目（清单中 `===== <name> =====` 起的一段）。
    ///
    /// 三字段与 §3.13 元素表逐条对应：组件名 / 许可名 / 版权行。
    struct LicenseEntry: Identifiable {
        /// 组件名，同时作 `id`（清单内唯一，由生成器按登记顺序输出）。
        let id: String
        /// 组件名（`headline`）。
        let name: String
        /// SPDX 许可标识符原文（`subhead` `txt.secondary`）——**不翻译**（U-L-4）。
        let license: String
        /// 版权行（`footnote` `txt.tertiary`）；清单中无 `Copyright` 行时为 `nil`。
        let copyright: String?
    }

    /// 清单解析结果。
    struct ParsedManifest {
        let entries: [LicenseEntry]
        /// 「尚未 vendor 的依赖」区的原文（无该区时为 `nil`）。
        let notVendoredNote: String?
        /// 文件整体是否读取成功（false → 页面如实提示，不伪造条目）。
        let loaded: Bool
    }

    // =======================================================================
    // MARK: - 解析（纯函数，可单测）
    // =======================================================================

    /// 解析清单文本（`cmake/Licenses.cmake` 的输出格式）。
    ///
    /// 格式（见 `recon_generate_licenses_manifest`）：
    /// ```
    /// ===== <name> =====
    /// <SPDX license>
    /// Upstream: <owner/repo>
    /// Ref: <ref>
    /// Subtree: <path>
    /// --- <LICENSE file> ---
    /// <全文>                      ← **尚未 vendor 的依赖没有这一段**（生成器
    ///                                在校验到无许可文件时 `continue`，但
    ///                                header/Upstream/Ref/Subtree **已写入**）
    ///
    /// ===== <next name> =====
    /// ...
    /// ===== 尚未 vendor 的依赖（清单暂缺，见 docs/12 §2.1 D-3）=====
    /// <说明 + 逐行 `  - <name>`>
    /// ```
    ///
    /// ── 判据为何锚 `Upstream:` 而不锚标题文字 ─────────────────────────────
    /// 结构性区分：**每个真实依赖条目都必含 `Upstream:` 行**（生成器对 12 项
    /// 依赖无条件写入，无论是否 vendor），而末尾「尚未 vendor」汇总区**没有**。
    /// 用这个判据而不是匹配标题的中文，一是更稳（标题措辞可能演进），
    /// 二是避免在源码里写一段**非 UI 文案的中文字面量**（G9 闸门要求视图层
    /// 中文必须经 `L10n` 取值；那是一段数据格式标记，本不该进 locale 表）。
    static func parse(_ text: String) -> ParsedManifest {
        // 先把文本切成「节」：每节 = 一个 `===== X =====` 标题 + 其后的正文行。
        struct Section { let title: String; var body: [String] }
        var sections: [Section] = []
        var current: Section?

        for rawLine in text.split(separator: "\n", omittingEmptySubsequences: false) {
            let line = String(rawLine)
            let trimmed = line.trimmingCharacters(in: .whitespaces)

            // 节标题行：行首行尾均为 `=====` 且中间有内容。
            if trimmed.hasPrefix("====="), trimmed.hasSuffix("====="),
               trimmed.count > 10 {
                let title = trimmed
                    .dropFirst(5)                    // 去前 "====="
                    .dropLast(5)                     // 去后 "====="
                    .trimmingCharacters(in: .whitespaces)
                if let cur = current { sections.append(cur) }
                current = title.isEmpty ? nil : Section(title: title, body: [])
                continue
            }
            current?.body.append(line)
        }
        if let cur = current { sections.append(cur) }

        // 分类：含 `Upstream:` 行的是真实依赖条目，否则是末尾汇总区。
        var entries: [LicenseEntry] = []
        var noteChunks: [String] = []

        for sec in sections {
            let hasUpstream = sec.body.contains {
                $0.trimmingCharacters(in: .whitespaces).hasPrefix("Upstream:")
            }
            if !hasUpstream {
                // 「尚未 vendor」汇总区（原文保留，供如实呈现）。
                let chunk = ([sec.title] + sec.body)
                    .joined(separator: "\n")
                    .trimmingCharacters(in: .whitespacesAndNewlines)
                if !chunk.isEmpty { noteChunks.append(chunk) }
                continue
            }

            // SPDX 许可名 = 正文里第一条非空行（生成器恒写这一行）。
            let license = sec.body
                .first { !$0.trimmingCharacters(in: .whitespaces).isEmpty }?
                .trimmingCharacters(in: .whitespaces) ?? ""
            // 版权行 = 正文里**首个**以 `Copyright` 开头的行（§3.13「版权行」）。
            // 未 vendor 的依赖没有许可全文，故取不到 —— 此时为 `nil`，UI 不占位。
            let copyright = sec.body
                .first { $0.trimmingCharacters(in: .whitespaces).hasPrefix("Copyright") }?
                .trimmingCharacters(in: .whitespaces)

            entries.append(LicenseEntry(id: sec.title, name: sec.title,
                                        license: license, copyright: copyright))
        }

        let note = noteChunks.isEmpty ? nil : noteChunks.joined(separator: "\n")
        return ParsedManifest(entries: entries, notVendoredNote: note, loaded: true)
    }

    // =======================================================================
    // MARK: - 读取包内清单（零网络；失败即如实标记，不伪造）
    // =======================================================================

    /// 从包**根**读 `THIRD_PARTY_LICENSES.txt`（扁平 bundle，无 `Contents/`）。
    ///
    /// 用 `Bundle.main.url(forResource:withExtension:)` —— 它对包根的非本地化
    /// 资源有效，且不需要手拼 `bundleURL`（后者在极端情况下受符号链接影响）。
    private static func loadManifest() -> ParsedManifest {
        guard let url = Bundle.main.url(forResource: "THIRD_PARTY_LICENSES",
                                       withExtension: "txt"),
              let text = try? String(contentsOf: url, encoding: .utf8)
        else {
            // 读不到：如实标记 loaded=false。**不**伪造条目、也不静默显示空清单
            // 冒充「依赖为零」。此状态在正常构建下**不应出现**（清单由
            // `gen_ios_project.py` 的构建脚本拷入包根，且 CI `build-app`
            // 有存在性断言 fail-closed）。
            return ParsedManifest(entries: [], notVendoredNote: nil, loaded: false)
        }
        return parse(text)
    }

    /// 清单在视图生命周期内**只解析一次**（文件是整个 App 生命周期不变的打包资源）。
    private static let manifest: ParsedManifest = loadManifest()

    private var parsed: ParsedManifest { Self.manifest }

    // =======================================================================
    // MARK: - 主体
    // =======================================================================

    public var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                introRow
                if !parsed.loaded {
                    // 读取失败：**如实**提示（不显示空清单冒充「没有依赖」）。
                    // 判据落在 `parsed.loaded` 而非 `entries.isEmpty` —— 后者在
                    // 「文件在但只含 not-vendored 区」时同样为空，两者语义不同。
                    unavailableRow
                } else {
                    entryList
                    if let note = parsed.notVendoredNote {
                        notVendoredRow(note)
                    }
                }
            }
            .padding(.vertical, ReconMetric.space4)
        }
        .background(ReconColor.bg.page)
        .navigationTitle(L10n.t(.licensesTitle))
        .navigationBarTitleDisplayMode(.inline)
    }

    // --- 首行引导说明（§3.13）---------------------------------------------

    private var introRow: some View {
        SpecText(L10n.t(.licensesIntro),
                 spec: ReconFont.footnote, color: ReconColor.txtSecondary,
                 maxLines: 0, applyCharLimit: false)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, ReconMetric.space4)
            .padding(.bottom, ReconMetric.space4)
    }

    // --- 组件条目列表（§3.13）---------------------------------------------

    private var entryList: some View {
        VStack(spacing: 0) {
            ForEach(Array(parsed.entries.enumerated()), id: \.element.id) { idx, entry in
                if idx > 0 { Hairline() }              // 条目之间 1px separator
                entryRow(entry)
            }
        }
    }

    private func entryRow(_ entry: LicenseEntry) -> some View {
        VStack(alignment: .leading, spacing: ReconMetric.space1) {
            // 组件名（`headline`）。
            SpecText(entry.name, spec: ReconFont.headline,
                     color: ReconColor.txtPrimary, maxLines: 2, applyCharLimit: false)
            // 许可名（`subhead` `txt.secondary`）—— **SPDX 标识符原文，不翻译**
            // （U-L-4：属「内容本身需保持 LTR」类，§6.5 L-4 例外）。
            SpecText(entry.license, spec: ReconFont.subhead,
                     color: ReconColor.txtSecondary, maxLines: 1, applyCharLimit: false)
            // 版权行（`footnote` `txt.tertiary`）；清单无该行时不占位。
            if let copyright = entry.copyright {
                SpecText(copyright, spec: ReconFont.footnote,
                         color: ReconColor.txtTertiary, maxLines: 2, applyCharLimit: false)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, ReconMetric.space4)
        .padding(.vertical, ReconMetric.space3)
        // §6.2 A11Y-2：陈述句，不重复控件类型（本行为静态文本，非按钮）。
        .accessibilityElement(children: .combine)
    }

    // --- 「尚未 vendor」如实说明（规格未定义呈现，见文件头登记）------------

    private func notVendoredRow(_ note: String) -> some View {
        // 原文来自清单文件（**内容本身**，非 UI 文案），故不经 locale 键 ——
        // 与许可全文同理（§6.5 L-4 类）。`InlineNotice` 的 expected 语义是
        // 「不隐藏失败」（U-D-7），与此处用途一致。
        InlineNotice(note, level: .info)
            .padding(.horizontal, ReconMetric.space4)
            .padding(.top, ReconMetric.space4)
    }

    // --- 读取失败（fail-closed，正常构建下不可达）-------------------------

    private var unavailableRow: some View {
        // 复用「如实告知未接入」的既有键（语义为「未接入/不可用」），**不新造键**
        // —— §5.9 未登记「清单读取失败」键，代码不先行。
        // ⚠️ 该状态在正常构建下**不应出现**：`gen_ios_project.py` 的构建脚本把
        //    清单拷入包根，CI `build-app` 亦有存在性断言（fail-closed）。
        InlineNotice(L10n.t(.resultPreviewPending), level: .warning)
            .padding(.horizontal, ReconMetric.space4)
    }
}
