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
    # a tree = the guide's tree + an empty .nojekyll, built in a scratch index
    # (no text piped through PowerShell: it would add CRLF to the entry names)
    $empty = (git hash-object -w -t blob (New-Item -ItemType File -Force (Join-Path $env:TEMP "svr2011_nojekyll"))).Trim()
    $env:GIT_INDEX_FILE = Join-Path $env:TEMP "svr2011_gh_pages.index"
    Remove-Item $env:GIT_INDEX_FILE -ErrorAction SilentlyContinue
    git read-tree $guide
    git update-index --add --cacheinfo "100644,$empty,.nojekyll"
    $tree = (git write-tree).Trim()
    Remove-Item Env:GIT_INDEX_FILE
    $head = (git rev-parse --short HEAD).Trim()
    $msg = "Mod Maker guide from $head"
    $parent = git rev-parse -q --verify gh-pages 2>$null
    $commit = if ($parent) { ($msg | git commit-tree $tree -p $parent.Trim()).Trim() } else { ($msg | git commit-tree $tree).Trim() }
    git update-ref refs/heads/gh-pages $commit
    "gh-pages -> $commit ($msg)"
    git ls-tree --name-only gh-pages
    if ($Push) { git push -f origin gh-pages }
} finally { Pop-Location }
