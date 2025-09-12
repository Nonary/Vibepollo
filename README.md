# Vibeshine

## What is Vibeshine?

Vibeshine is a Windows-exclusive fork of *Sunshine* designed to introduce significant enhancements, including:

- **API token management**
- **Session-based authentication**
- **A fully redesigned frontend with complete mobile support**
- **Playnite integration**
- **Windows Graphics Capture running in service mode**
- **Update notifications**
- **Numerous bug fixes**

Due to the sheer pace and volume of changes I was producing, it became impractical to manage them within the original Sunshine repository. The review process simply couldn’t keep up with the rate of development, and large feature sets were piling up without a clear path to integration. To ensure the work remained organized, maintainable, and actively progressing, I established Vibeshine as a standalone fork.

Currently, Vibeshine has already introduced over **30,000 new lines of code**, nearly matching the size of Sunshine’s original codebase.

---

## Does Vibeshine aim to replace Sunshine?

No. Vibeshine is intended as a **complementary fork**, not a replacement. It also intends to incorporate functionality from *Apollo* in the future.

---

## Will Vibeshine's Features Merge Back Into Sunshine or Apollo?

Short answer: Unlikely to be backported.

Vibeshine is largely AI-generated. While it works well, it carries a kind of surface-level technical debt that many upstream projects want resolved before taking big changes. This includes inconsistent styling, thin or missing documentation, and some over-engineering. I personally see this type of debt as unimportant today. Modern AI tools make it easy to ask questions like “why does this function exist?”, “what does this parameter do?”, or “how do these classes interact?” and get immediate, accurate answers. Soon, AI will even be able to auto-fix these issues—re-style entire trees, write docstrings, and remove unused layers—without human effort.

So this “mess” is only cosmetic. It doesn’t break the code, create security risks, or block future maintenance. The only debt that truly matters is architectural decisions: API design, threading models, modularity, and performance. These are the parts that create long-term problems, and are much harder to fix even with AI tools. That’s why I focus on making those decisions up front, guiding the AI on how to build the code.

Because I define the architecture, I know how everything works. Whether the code looks polished or not doesn’t matter to me. 

Bringing Vibeshine fully in line with upstream style and documentation standards would take a lot of engineering time for very little practical gain. For now, backporting is unlikely. The fork will continue to move quickly here, and over time, targeted refactors or added documentation may make selective upstreaming possible.
---


## Origin of the Name "Vibeshine"

The name arose as a playful suggestion from another developer who joked about the potential unmanageability of extensive AI-generated code. Given that approximately **99% of Vibeshine’s code is AI-generated**, the name seemed fitting.

---

## Why Use AI-generated Code? Concerns About Technical Debt?

AI significantly accelerates development by offloading much of the routine implementation work. Instead of spending hours writing boilerplate, wiring dependencies, or handling repetitive edge cases, I can focus on high-level architecture, long-term design decisions, and system direction. This shift doesn’t just speed things up—it fundamentally changes the role of the engineer, pushing us toward oversight, orchestration, and design rather than rote code production.

What stands out most is that AI code works on the first try around 90% of the time. That reliability, combined with instant generation, makes it dramatically more efficient to accept its form of debt than to painstakingly write everything from scratch. In other words, I’m trading minor, manageable debt for massive development velocity—and that trade is almost always worth it.

I’m not overly concerned about technical debt in this workflow, because the debt that truly matters stems from bad architecture and poor design choices, not from the code itself. As long as I guide the AI with clear structure and intent, the generated code ends up being maintainable. Problems like inconsistent naming, redundant code, or unused helpers are minor forms of debt—easily identified, cleaned up, or ignored. By contrast, deep architectural flaws, poor layering, or mismatched abstractions create lasting problems.


In fact, compared to many traditional enterprise codebases I’ve maintained, AI-assisted code often comes out cleaner and easier to manage. Legacy systems are usually burdened with years of ad-hoc patches, inconsistent styles, and various bad practices due to knowledge level of contributor. AI-generated code doesn’t necessarily carry fewer design flaws than human code, but it does avoid accumulating those scars—especially when paired with an intentional architectural vision and it is less likely to do seriously bad practices that you typically find in enterprise codebases.

Broadly speaking, AI-assisted development represents the future of software engineering. Just as compilers and IDEs once transformed programming, AI is now transforming how we design, implement, and maintain systems. Instead of fearing it, I view it as a force multiplier that complements professional judgment. Vibeshine is an example of what happens when you embrace that shift: rapid iteration, a massive expansion of features, and code that remains maintainable because the architecture is intentionally guided.

---

## AI Models Used by Vibeshine

Vibeshine primarily leverages:

- **GPT-5 (high/medium reasoning)** via Codex CLI on a ChatGPT Pro subscription:
  - Medium reasoning for most code generation.
  - High reasoning for complex or challenging features.

- **GPT-5 mini** via Visual Studio Code:
  - Handles minor tasks such as formatting and documentation.
  - Fast and cost-effective, available with unlimited usage on the $10 GitHub Copilot plan.
  - Nearly matches Claude Sonnet 4 in performance (only 6% lower on SWE Bench), surpassing GPT 4.1.

Previously, I relied heavily on **Claude Sonnet 4**, which had some limitations:
- Frequently strayed off my architectural plan
- Rarely challenged the developer on prompts, would do anything you told it even if you were wrong.
- Fast paced, but often would write bad code and correct it as it is working.

In contrast, GPT-5:
- Actively double checks your inquiry and confidently tells you you're wrong; making it feel like a true AI companion in development process.
- Thinks much longer up front to ensure that the code it writes is accurate and bug free and fits requirements.
- Holistically understands the codebase, puts thought to how requested changes can impact the existing codebase.
- Is able to answer just about any question in a codebase, from how a feature works to how to add a new feature.
