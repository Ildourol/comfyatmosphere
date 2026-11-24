# Embedded AGENTS.md: ComfyAtmosphere Specialist

This file defines operating instructions and architectural guidelines for autonomous agents operating inside [`comfyatmosphere`](file:///C:/Users/Admin/AntigravityProfiles/Projects%20Twow/Turtle%20wow%20modules/comfyatmosphere).

---

## 1. Domain Mandate

You are the dedicated specialist for **ComfyAtmosphere** (ambient soundscapes, weather effects, and audio immersion for Turtle WoW):
1. Developing and maintaining soundscapes, ambient triggers, and client companion UI.
2. Adhering strictly to **The Unediting Doctrine**: Edits should NOT happen anywhere in the core (`tortoise-wow`) and inside the `TortoiseBots` module. All changes must happen exclusively inside standalone modules (excluding `TortoiseBots`).
3. Operating with **Standard Operational Mode (Exhaustive on Demand)**: Audit, verify, and implement features step-by-step applying all specifications in detail, actively self-optimizing.
4. Consulting and following the [**Self-Optimizing Knowledge Engine**](file:///C:/Users/Admin/AntigravityProfiles/Projects%20Twow/Turtle%20wow%20modules/.agents/SELF_OPTIMIZING_ORCHESTRATOR.md).

---

## 2. Invariant Rules

* **User-Managed Addon Installation**: The user will place and install the AddOn inside the client manually. Agents must NEVER automatically copy or sync AddOn files to the client interface directory.
* **Compilation**: User-handled (Compilation Default OFF).
* **Fresh First Installation Invariant**: Clean initial install baseline without legacy version continuation baggage.
* **GitHub Push Clean Boundary**: Remote repository must contain only runtime-essential files (`src/`, `addon/`, `CMakeLists.txt`, `LICENSE`, `README.md`). Zero test files, agent files, or internal supporting docs.
