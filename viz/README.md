# Visual guide

These static pages explain what "integer-only" means in this project and how
it relates to traditional fixed-point celestial-navigation work.

- `index.html` - overview, scope of the claim, limitations, and links to the
  three detailed pages.
- `three-ages.html` - one sight worked with log tables, conventional floating
  point, and the library's integer CORDIC/vector methods.
- `human-machine.html` - recorded sextant values, scaled-integer encodings,
  binary64 representation, quantization, and reproducibility checks.
- `end-to-end.html` - a guided tour from time and almanac data through sight
  correction, reduction, fixes, timed crossings, and a synthetic Sun sight.

Open `index.html` directly, or use the published entry point at
<https://nmicic.github.io/astro-nav-int/viz/>. The pages require no build step
or external JavaScript libraries.

For the underlying integer math routines, see the companion
[`fp_math.h` visualization](https://nmicic.github.io/int-llm/viz/). Source,
tests, and validation details are in the repository's main `README.md`.
