<p align="center">
  <a href="https://nodejs.org/">
    <img
      alt="Node.js"
      src="https://nodejs.org/static/images/logo-light.svg"
      width="400"
    />
  </a>
</p>

Node.js is a JavaScript runtime built on Chrome's V8 JavaScript engine. For
more information on using Node.js, see the
[Node.js Website](https://nodejs.org/).

## Node in Electron

#### Overview 

The Electron Project embeds Node, which allows developers to leverage all of Node's capabilities and access the filesystem on your desktop platform. Electron embeds different versions of Node in different release lines of Electron. These versions are chosen such that they depend on a version of V8 compatible with the version of V8 present in the Chromium version used for that release line.

#### Branching Strategy:
`master` in this fork is an unused branch; a version of Node present in a release line can be found in a branch with the naming scheme
`electron-node-vX.Y.Z`.

|  | 5-0-x | 4-0-x | 3-0-x | 2-0-x | 1-8-x | 1-7-x |
|---|---|---|---|---|---|---|
| Chromium | `v72.0.3626.52` | `v69.0.3497.106` | `v66.0.3359.181` | `v61.0.3163.100` | `v59.0.3071.115` | `v58.0.3029.110` |
| Node | [`v12.0.0-unreleased`][node50x] | [`v10.11.0`][node40x] | [`v10.2.0`][node30x] | [`v8.9.3`][node20x] | [`v8.2.1`][node18x] | [`v7.9.0`][node17x] |
| V8 | `7.2.502.19` | `v6.9.427.24` | `v6.6.346.23` | `v6.1.534.36` | `v5.8.283.38` | `v5.5.372.40` |

See [our website](https://electronjs.org) for what versions of Node are present in which release lines.

#### Working on the fork

To make changes to Node for a specific version of Electron, see `electron/vendor/node` for the version of Node in that release line, and then open a Pull Request against the associated `electron-node-vX.Y.Z` branch.

[node17x]: https://github.com/electron/node/tree/electron-node-v7.9.0
[node18x]: https://github.com/electron/node/tree/electron-node-v8.2.1
[node20x]: https://github.com/electron/node/tree/electron-node-v8.9.3
[node30x]: https://github.com/electron/node/tree/electron-node-v10.2.0
[node40x]: https://github.com/electron/node/tree/electron-node-v10.11.0-V8-6.9
[node50x]: https://github.com/electron/node/tree/electron-node-v12.x
