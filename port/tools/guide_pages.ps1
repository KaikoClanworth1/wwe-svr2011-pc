# The Mod Maker guide (docs/guide) as the gh-pages branch, for GitHub Pages:
# the branch holds only the guide (index.html at its root, img\), nothing else
# of the repository, so it can be pushed on its own while main waits for a
# release. Rebuilds the branch from the committed docs/guide of the current
# HEAD; a .nojekyll file keeps Pages from running Jekyll over it.
#
#   port\tools\guide_pages.ps1            rebuild gh-pages from HEAD's docs/guide
#   port\tools\guide_pages.ps1 -Push      ...and push it (git push -f origin gh-pages)
#
# Pages itself is turned on once, in the repository's Settings > Pages:
# Source "Deploy from a branch", branch gh-pages, folder / (root) - or
#   gh api -X POST repos/KaikoClanworth1/wwe-svr2011-pc/pages -f "source[branch]=gh-pages" -f "source[path]=/"
# The guide is then at https://kaikoclanworth1.github.io/wwe-svr2011-pc/
param([switch]$Push)
$ErrorActionPreference = "Stop"
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $root
try {
    $guide = (git rev-parse "HEAD:docs/guide").Trim()
    if (-not $guide) { throw "HEAD has no docs/guide" }
    # a tree = the guide's tree + an empty .nojekyll
    $empty = ("" | git hash-object -w --stdin).Trim()
    $lines = (git ls-tree $guide) + "100644 blob $empty`t.nojekyll"
    $tree = ($lines -join "`n" | git mktree).Trim()
    $head = (git rev-parse --short HEAD).Trim()
    $msg = "Mod Maker guide from $head"
    $parent = git rev-parse -q --verify gh-pages 2>$null
    $commit = if ($parent) { ($msg | git commit-tree $tree -p $parent.Trim()).Trim() } else { ($msg | git commit-tree $tree).Trim() }
    git update-ref refs/heads/gh-pages $commit
    "gh-pages -> $commit ($msg)"
    git ls-tree --name-only gh-pages
    if ($Push) { git push -f origin gh-pages }
} finally { Pop-Location }
