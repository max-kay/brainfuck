#let tape(
  ..cells,
  ptr: 0,
  size: 4em,
  edge: 3.4em,
  accent: rgb("#e8590c"),
  caret: true,
  r: 0.4em,
) = {
  let cells = cells.pos()
  let n = cells.len()
  let gray = luma(210)
  let transp = gray.transparentize(100%)

  let left-edge = rect(
    width: 90%,
    height: 90%,
    radius: (top-right: r, bottom-right: r, rest: 0pt),
    fill: gradient.linear(transp, gray, angle: 0deg),
  )
  let right-edge = rect(
    width: 90%,
    height: 90%,
    radius: (top-left: r, bottom-left: r, rest: 0pt),
    fill: gradient.linear(gray, transp, angle: 0deg),
  )

  let cell(c, i) = rect(
    width: 90%,
    height: 90%,
    fill: gray,
    radius: r,
    stroke: if i == ptr { accent + 1.4pt } else { none },
    align(center + horizon, c),
  )

  let carets = range(n + 2).map(j => {
    if caret and j == ptr + 1 {
      align(center + top, text(fill: accent, size: 0.9em)[▲])
    } else { [] }
  })

  align(center, grid(
    columns: (edge, ..((size,) * n), edge),
    rows: (size, auto),
    row-gutter: 0.15em,
    left-edge,
    ..cells.enumerate().map(((i, c)) => cell(c, i)),
    right-edge,
    ..carets,
  ))
}

#let transform(
  before,
  code,
  after,
  accent: rgb("#e8590c"),
  gutter: 1em,
  fill: luma(240),
  inset: 10pt,
  radius: 4pt,
) = {
  // Re-render as non-block raw so the document-wide
  // `raw.where(block: true)` rule doesn't match it.
  let inner = if code.func() == raw {
    raw(code.text, lang: code.at("lang", default: none), block: false)
  } else { code }

  // Our own gray box, sized to its content
  let boxed = box(
    fill: fill,
    inset: inset,
    radius: radius,
    align(left, inner),
  )

  grid(
    columns: 3,
    column-gutter: gutter,
    align: horizon + center,
    before,
    stack(
      dir: ttb,
      spacing: 0.4em,
      boxed,
      text(size: 1.6em)[$arrow.r.long$],
    ),
    after,
  )
}

#let transform_vertical(
  ..args,
  accent: rgb("#e8590c"),
  gutter: 0.8em,
  fill: luma(240),
  inset: 10pt,
  radius: 4pt,
  arrow-gap: 1em,
) = {
  let items = args.pos()
  let count = items.len()

  assert(
    count >= 3 and calc.odd(count),
    message: "transform_chain expects 2n+1 positional arguments (n >= 1): stage, code, stage, ..., stage; got "
      + str(count),
  )

  // Inline raw so a document-wide block rule doesn't match
  let as-inline(code) = if code.func() == raw {
    raw(code.text, lang: code.at("lang", default: none), block: false)
  } else { code }

  let step(code) = stack(
    dir: ltr,
    spacing: arrow-gap,
    text(size: 1.6em, fill: accent)[$arrow.b$],
    box(
      fill: fill,
      inset: inset,
      radius: radius,
      align(left, as-inline(code)),
    ),
  )

  // Even indices are stages, odd indices are transformations
  let parts = items
    .enumerate()
    .map(((i, item)) => {
      if calc.even(i) { item } else { step(item) }
    })

  align(center, stack(
    dir: ttb,
    spacing: gutter,
    ..parts,
  ))
}
