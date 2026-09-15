# Lighting, Liquids, Portals, Fireworks & Effects (Web Port)

**Mostly identical to the root docs — see
[`../../docs/lighting-liquids-effects.md`](../../docs/lighting-liquids-effects.md).**
Liquid flow, portals, fireworks, and particle systems are unmodified engine code.

## The one addition
The GL shim tracks `GL_LIGHT0` ambient/diffuse (`glLightfv`) — but applies it **only**
on the matrix-palette creature-skinning shader path (see
[gl-shim.md](gl-shim.md), [entities-and-creatures.md](entities-and-creatures.md)).
General fixed-function lighting for doors and the golden cube's specular highlight is
still unimplemented in the shim — a deliberately narrow gate chosen to avoid touching
already-verified rendering paths, not an oversight.
