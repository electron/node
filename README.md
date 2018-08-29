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
[Node.js Website](https://http://nodejs.org/).

## Node in Electron

#### Overview 

The Electron Project embeds Node, which allows developers to leverage all of Node's capabilities and access the filesystem on your desktop platform. Electron embeds different versions of Node in different release lines of Electron. These versions are chosen such that they depend on a version of V8 compatible with the version of V8 present in the Chromium version used for that release line.

#### Branching Strategy:
`master` in this fork is an unused branch; a version of Node present in a release line can be found in a branch with the naming scheme
`electron-node-vX.Y.Z`.

|  | 1-7-x | 1-8-x | 2-0-x | 3-0-x |
|---|---|---|---|---|
| Chromium  | `v58.0.3029.110` | `v59.0.3071.115` | `v61.0.3163.100` | `v66.0.3359.181` |
| Node | `v7.9.0` | `v8.2.1` | `v8.9.3` | `v10.2.0` |
| V8 | `v5.5.372.40` | `v5.8.283.38`  | `v6.1.534.36` | `v6.6.346.23` |

See [our website](https://electronjs.org) for what versions of Node are present in which release lines.

#### Working on the fork

To make changes to Node for a specific version of Electron, see `electron/vendor/node` for the version of Node in that release line, and then open a Pull Request against the associated `electron-node-vX.Y.Z` branch.
