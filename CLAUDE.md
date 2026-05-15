# Claude Code Instructions

## Philosophy

1. **Domain driven development**. Model the software around the business domain, not around
   technology layers. The domain is the source of truth; code is its expression.
   - Ubiquitous Language: One shared vocabulary across domain experts, designers, and code. Class
     names, method names, and module names mirror the terms domain experts already use. If the term
     changes in conversation, it changes in the code in the same PR. No translation layer.
   - Bounded Context: Each model is valid only inside an explicit boundary. The same word
     ("entity", "asset", "scene") may mean different things in different contexts; do not unify them
     into one god-model. Make the boundary a hard line — separate modules, separate types, explicit
     translation at the seam (anti-corruption layer).
   - Context Map: Document how bounded contexts relate (upstream/downstream, shared kernel,
     customer/supplier, conformist, anti-corruption). Integration shape is a design decision, not
     an accident of import order.
   - Strategic before tactical: Find the *core domain* (the part where the engine wins) and spend
     the design budget there. Generic and supporting subdomains get plain, boring solutions —
     reuse, buy, or thinnest-possible code. Do not lavish DDD machinery on incidental work.
   - Tactical building blocks (use when they earn their keep, not by reflex):
     - Entity: identity persists across state changes; equality by ID.
     - Value object: immutable, equality by attributes, no identity. Prefer these — they collapse
       a class of bugs.
     - Aggregate: a consistency boundary with one root. External code touches the root only;
       invariants hold inside the boundary, transactions never cross it. Keep aggregates small.
     - Domain event: a fact that happened in the domain, named in past tense, that other parts of
       the system react to. Use to decouple aggregates instead of forcing one big transaction.
     - Repository: collection-like access to aggregates; hides persistence. One repository per
       aggregate root.
     - Domain service: behavior that genuinely belongs to the domain but does not fit a single
       entity or value object. Not a dumping ground for procedural code.
   - Keep the domain pure: domain types know nothing about IO, frameworks, serialization, or UI.
     Push side effects to the edges (hexagonal / ports-and-adapters). The domain layer must be
     testable with no engine, no renderer, no filesystem.
   - Refactor toward deeper insight: the model is never finished. When a new requirement makes the
     current model awkward, that is signal — rename, split aggregates, redraw boundaries. Record
     the change in the design doc and update the ubiquitous language in the same PR.
2. **SOLID**
   - S – Single Responsibility Principle (SRP): A class should have only one reason to change.
     Every class or module should focus on performing one specific job or task.
   - O – Open/Closed Principle (OCP): Software entities should be open for extension but closed for
     modification. You should be able to add new functionality without changing existing code.
   - L – Liskov Substitution Principle (LSP): Subtypes must be completely replaceable for their base
     types without altering the correctness of the program.
   - I – Interface Segregation Principle (ISP): No client should be forced to depend on methods it
     does not use. It is better to have many small, specific interfaces than one large,
     general-purpose one.
   - D – Dependency Inversion Principle (DIP): High-level modules should not depend on low-level
     modules; both should depend on abstractions. Depend on interfaces rather than concrete
     implementations.
3. **Occam's razor at every decision**. Two collapsing requirements become one primitive. Record
   the collapse in the spec.
4. **Murphy's Law**. What can go wrong, will go wrong. Challenge every assumption. Code defensively.
   Address all edge cases and potential failure scenarios.
5. **Simple, not easy**. Simplicity is a choice: You must develop an "entanglement radar" to spot
   when you are needlessly braiding components together, and actively avoid it. Focus on the
   artifact: The goal isn't just to make the act of programming feel easy, but to ensure the
   resulting artifacts (the system) are simple and robust. Tolerate change: Simple systems support
   independent development, allowing parts of a system to evolve without requiring the whole system
   to be constantly rewritten or reconfigured.
   - Cohesiveness (Focus on One Thing): High cohesion means each component has one clear, focused
     purpose. Hickey emphasizes that a simple system lacks interleaving—meaning its components are
     not mixed or muddled together. When you break code into simple, single-purpose pieces, you
     maximize cohesiveness because each piece does one job and does it well.
   - Coupling (Avoiding Entanglement): Coupling is the degree to which modules depend on each other.
     Hickey warns heavily against "complecting," which happens when independent parts are tightly
     bound together. Low coupling means components make minimal assumptions about each other,
     allowing you to reason about them independently without holding the entire system in your head.
