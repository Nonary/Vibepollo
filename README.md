# Vibeshine

## What is Vibeshine?

Vibeshine is an AI‑enhanced version of Sunshine, a popular remote streaming application. It intends to integrate all scripts from myself (Nonary) and more.



## Key Features

* **Display Setting Automation**
  Vibeshine adds multiple safeguards to prevent dummy plugs or virtual displays from getting “stuck” when you return to your PC. It resolves common Windows 11 **24H2** display issues and restores your layout after hard crashes, shutdowns, or reboots. (The only scenario it can’t restore is during a user logout.) The workflow is simplified to a dropdown—just pick the display you want to stream.

* **Windows Graphics Capture in Service Mode**
  Running Windows Graphics Capture (WGC) as a service improves performance and stability. It captures the full frame rate of frame‑generated titles, avoids crashes when VRAM is exceeded, and follows Microsoft’s recommended capture method going forward. Vibeshine auto‑switches capture methods on demand, so the login screen and UAC prompts are still captured even when using WGC.

* **Native Virtualized Display**
  Vibeshine includes SudoVDA by default, with multiple stability improvements. It can capture output from any GPU, including those in hybrid laptops, ensuring the virtual screen connects to the correct GPU when needed. It also provides simple virtual display options, allowing users to choose between a physical or virtual display. On headless setups, it enables automatically to prevent 503 errors and false encoder detections, such as incorrect HEVC support reports.

* **WebRTC Browser Streaming**
  Vibeshine can stream straight to your web browser from the `/webrtc` page, so you can play without installing a separate client. It is designed for fast response and smooth audio/video, while still letting you use the regular Moonlight-compatible streaming path if you prefer.

* **Redesigned Frontend with Full Mobile Support**
  The new Web UI makes it easy to add games and change settings without restarting the program. It’s fully responsive, so you can manage your library and configuration from a phone or tablet.

* **Playnite Integration**
  Deep integration with Playnite (a “launcher of launchers”) automatically syncs your recently played games with configurable expiration rules, per‑category sync, and exclusions. You can also add games manually from a Web UI dropdown; Vibeshine handles artwork, launching, and clean termination—emulators included. The goal is a seamless, GeForce Experience–style library experience—only better.

* **RTSS & NVIDIA Control Panel Integration**
  Vibeshine can manage RTSS to apply the correct frame limit and disable V‑Sync before streaming, significantly improving frame pacing and smoothness. The applied frame cap matches the client device’s requested FPS.

* **Frame‑Generated Capture Fixes**
  Vibeshine includes workarounds so DLSS/FSR frame‑generated games are captured at the game’s full frame rate without micro‑stutter. This requires a very high‑refresh‑rate display (physical or virtual) at **240 Hz**.

* **Lossless Scaling & NVIDIA Smooth Motion**
  Vibeshine can automatically apply optimal Lossless Scaling settings to generate frames for any application. On RTX 40‑series and newer GPUs, you can optionally enable **NVIDIA Smooth Motion** for better performance and image quality (while Lossless Scaling remains more customizable).

* **API Token Management**
  Access tokens can be tightly scoped—down to specific methods—so external scripts don’t need full administrative rights. This improves security while keeping automation flexible.

* **Session‑Based Authentication**
  The sign‑in flow supports password managers and includes a “remember me” option to minimize prompts. The experience is security‑hardened without sacrificing convenience.

* **Update Notifications**
  Built‑in notifications let you know when new features or bug fixes are available, making it easy to stay current.

Due to the sheer pace and volume of changes I was producing, it became impractical to manage them within the original Sunshine repository. The review process simply couldn’t keep up with the rate of development, and large feature sets were piling up without a clear path to integration. To ensure the work remained organized, maintainable, and actively progressing, I established Vibeshine as a standalone fork.

At this point, Vibeshine differs from upstream Sunshine by roughly **99,800 changed lines** (about **91,800 added** and **8,000 removed**). At that scale, asking upstream maintainers to accept large backports in one sweep is generally not sustainable, which is why Vibeshine continues as a standalone fork.

---

## Does Vibeshine aim to replace Sunshine or Apollo?

No. Vibeshine is intended as a **complementary fork**, not a replacement.

In addition, for users who prefer Apollo’s ecosystem, there is a [Vibepollo](https://github.com/Nonary/Vibepollo) that brings Vibeshine’s feature set to Apollo. This exists primarily to serve Apollo users who asked for Vibeshine‑style capabilities while staying on Apollo.


## Will Vibeshine’s features merge back into Sunshine or Apollo?

**Short answer: Unlikely to be backported upstream as large, sweeping merges.**

Vibeshine is largely AI‑generated. While it works well, it carries a kind of surface‑level technical debt that many upstream projects want resolved before taking big changes (styling consistency, thin/missing docs, and some over‑engineering). I see that debt as relatively unimportant today because modern AI tools can answer “why does this function exist?”, “what does this parameter do?”, or “how do these classes interact?” and will soon auto‑fix these issues—re‑style trees, write docstrings, and prune unused layers—without human effort.

So this “mess” is mostly cosmetic. It doesn’t break the code, create security risks, or block future maintenance. The only debt that truly matters is architectural: API design, threading models, modularity, and performance. Those are hard to fix even with AI tools, which is why I focus on them up front and guide the AI accordingly.

Because I define the architecture, I know how everything works. Whether the code looks polished or not doesn’t matter to me.

Bringing Vibeshine fully in line with upstream style and documentation would take a lot of engineering time for limited practical gain. For now, full backports into Sunshine or Apollo are unlikely. Over time, targeted refactors or added documentation may make **selective upstreaming** possible.

**Note on Apollo:** The Apollo port is a downstream **backport**, not an upstream merge. It adapts Vibeshine features to Apollo’s architecture where it makes sense, with the understanding that Apollo maintains its own priorities and conventions.

---

## Origin of the Name "Vibeshine"

The name arose as a playful suggestion from another developer who joked about the potential unmanageability of extensive AI‑generated code. Given that approximately **99% of Vibeshine’s code is AI‑generated**, the name seemed fitting.

---

## Why Use AI‑generated Code? Concerns About Technical Debt?

AI significantly accelerates development by offloading much of the routine implementation work. Instead of spending hours writing boilerplate, wiring dependencies, or handling repetitive edge cases, I can focus on high‑level architecture, long‑term design decisions, and system direction. This shift doesn’t just speed things up—it fundamentally changes the role of the engineer, pushing us toward oversight, orchestration, and design rather than rote code production.

What stands out most is that AI code works on the first try around 90% of the time. That reliability, combined with instant generation, makes it dramatically more efficient to accept its form of debt than to painstakingly write everything from scratch. In other words, I’m trading minor, manageable debt for massive development velocity—and that trade is almost always worth it.

I’m not overly concerned about technical debt in this workflow, because the debt that truly matters stems from bad architecture and poor design choices, not from the code itself. As long as I guide the AI with clear structure and intent, the generated code ends up being maintainable. Problems like inconsistent naming, redundant code, or unused helpers are minor forms of debt—easily identified, cleaned up, or ignored. By contrast, deep architectural flaws, poor layering, or mismatched abstractions create lasting problems.

In fact, compared to many traditional enterprise codebases I’ve maintained, AI‑assisted code often comes out cleaner and easier to manage. Legacy systems are usually burdened with years of ad‑hoc patches, inconsistent styles, and various bad practices due to knowledge level of contributor. AI‑generated code doesn’t necessarily carry fewer design flaws than human code, but it does avoid accumulating those scars—especially when paired with an intentional architectural vision, and it is less likely to do seriously bad practices that you typically find in enterprise codebases.

Broadly speaking, AI‑assisted development represents the future of software engineering. Just as compilers and IDEs once transformed programming, AI is now transforming how we design, implement, and maintain systems. Instead of fearing it, I view it as a force multiplier that complements professional judgment. Vibeshine is an example of what happens when you embrace that shift: rapid iteration, a massive expansion of features, and code that remains maintainable because the architecture is intentionally guided.

---

## The Original “AI-Only” Goal (And Why It Changed)

One of the original goals of Vibeshine was to prove a specific point: that an experienced developer could maintain a complex project using almost entirely AI‑generated code, as long as they provided the architecture and kept the system coherent.

That idea hasn’t aged particularly well, not because it was wrong, but because the models scaled far faster than most projections. Most of Vibeshine was developed on GPT‑5, which (per METR’s public benchmark on metr.org at the time) only hit about a 50% success rate on 1‑hour tasks. A few months later, GPT‑5.2 landed and the same 50% point moved out to roughly 6.5‑hour tasks, which is a huge jump in practical autonomy.

The result is that the “skill gap” in prompting and guiding the AI matters less than it did even a few months prior. You still need engineering judgment and architecture, but it’s now dramatically easier to get high‑quality, end‑to‑end results without the same level of careful orchestration. So the original “prove it’s possible” goal is basically moot: it’s not a niche workflow anymore, it’s simply where the tools have gone.

---

## AI Models Used by Vibeshine

Vibeshine has always been built with **Codex** as the primary workflow, and in practice that has meant mostly the **GPT‑5 family** (today: **GPT‑5.3‑Codex**). I use it with the same principles as before: start from architecture, sanity‑check assumptions, and do the hard reasoning up front so the implementation lands cleanly.

With **GPT‑5.3‑Codex**, there’s no real need to juggle a “fast but less capable” model anymore. In the past I’d reach for speed‑first models (like Sonnet, or smaller GPT “mini” variants) for quick turnaround, but **GPT‑5.3‑Codex** covers both: it’s about as fast as those options while also being strong enough to handle the hard engineering work in one pass.

Claude was used more heavily earlier on. Older Claude models had a tendency to go off on their own path, even when the architectural plan was clear. That behavior has mostly been fixed in newer Claude releases, but GPT still ended up being the more useful engineering tool for me because it will challenge you and not simply agree with whatever you ask for.

In general, GPT has felt more intelligent for the way I build and maintain this codebase. I may occasionally ask **Claude Opus 4.5** for a second opinion if GPT can’t resolve something cleanly end‑to‑end, but this is increasingly rare.
