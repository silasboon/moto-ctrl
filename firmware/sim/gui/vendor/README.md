# vendor/

- `nacl.min.js` — [TweetNaCl.js](https://github.com/dchest/tweetnacl-js) 1.0.3,
  the browser build (`node_modules/tweetnacl/nacl.min.js` from
  `firmware/sim/itest`'s pinned dependency, copied verbatim). Public domain
  (Unlicense — see `nacl.LICENSE`). Loaded via a plain `<script>` tag with no
  bundler, attaching `window.nacl`, so the GUI has no build step and no
  network dependency (this project is offline-first, ever — that includes
  dev tools). Interoperates with the C TweetNaCl used by the firmware/sim core
  (`firmware/components/core/vendor/tweetnacl/`) and with the Node reference
  client (`firmware/sim/itest/moto-client.mjs`), which uses the same
  package.

Do not hand-edit `nacl.min.js`. To update, bump the version in
`firmware/sim/itest/package.json`, run `npm install` there, and re-copy
`node_modules/tweetnacl/nacl.min.js`.
