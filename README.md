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

## Will Vibeshine's Features Merge Back Into Sunshine?

**Short Answer:** Not currently planned.

Active contributions to Sunshine are paused due to unresolved governance issues within the original project.

---

## Reasons for Pausing Contributions

1. **Inconsistent Merge Practices:**  
   The project owner frequently merges their own breaking changes without proper reviews or reversions, insisting on a "fix-forward" approach while holding others to higher standards.

2. **Restricted Access:**  
   My access to the Sunshine repository is banned, preventing participation in pull requests or discussions.

3. **Centralized Control:**  
   The project owner emphasizes complete personal control, despite Sunshine's community-driven intent.

4. **Unannounced Repository Changes:**  
   Major changes to the repository organization structure have occurred without prior notification such as abruptly revoking permissions from existing contributors.

5. **Policy Changes Without Community Input:**  
   Contribution guidelines and codes of conduct have been modified without open community discussion or consensus.

6. **Delayed Code Reviews:**  
   Pull requests are reviewed slowly by a limited group, restricting broader community participation.

7. **Suppression of Dissent:**  
   Comments critical of the project owner are regularly deleted, flagged as off-topic, or marked as spam, suppressing open debate.

8. **Limited Access to Repository Tools:**  
   Contributors lack basic tools like issue labeling, which are restricted to one or two individuals.

9. **Retaliation Against Dissent:**  
   Contributors challenging decisions have been banned or had their contributions halted, with the owner unwilling to reconsider once decisions are made.
---

## What About Apollo?

Yes, select Vibeshine features will eventually be integrated into *Apollo*. However, Apollo has significantly diverged from Sunshine, and integrating its features into Vibeshine will require considerable effort.

---

## Origin of the Name "Vibeshine"

The name arose as a playful suggestion from another developer who joked about the potential unmanageability of extensive AI-generated code. Given that approximately **99% of Vibeshine’s code is AI-generated**, the name seemed fitting.

---

## Why Use AI-generated Code? Concerns About Technical Debt?

AI significantly accelerates development by offloading much of the routine implementation work. Instead of spending hours writing boilerplate, wiring dependencies, or handling repetitive edge cases, I can focus on high-level architecture, long-term design decisions, and system direction. This shift doesn’t just speed things up—it fundamentally changes the role of the engineer, pushing us toward oversight, orchestration, and design rather than rote code production.

What stands out most is that AI code works on the first try around 90% of the time. That reliability, combined with instant generation, makes it dramatically more efficient to accept its form of debt than to painstakingly write everything from scratch. Even if I need to correct style inconsistencies afterward, the productivity gain is enormous. In other words, I’m trading minor, manageable debt for massive development velocity—and that trade is almost always worth it.

I’m not overly concerned about technical debt in this workflow, because the debt that truly matters stems from bad architecture and poor design choices, not from the code itself. As long as I guide the AI with clear structure and intent, the generated code ends up being structured and maintainable. Problems like inconsistent naming, redundant code, or unused helpers are minor forms of debt—easily identified, cleaned up, or ignored. By contrast, deep architectural flaws, poor layering, or mismatched abstractions create lasting problems.

In practice, most of the issues I encounter with AI-generated code are related to coding style, naming, or structure rather than deep architectural mistakes. Those are relatively minor concerns—easy to clean up later, and far less damaging than large-scale design flaws baked into a system from the start.

In fact, compared to many traditional enterprise codebases I’ve maintained, AI-assisted code often comes out cleaner and easier to manage. Legacy systems are usually burdened with years of ad-hoc patches, inconsistent styles, and undocumented hacks. AI-generated code doesn’t necessarily carry fewer design flaws than human code, but it does avoid accumulating those scars—especially when paired with an intentional architectural vision.

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
