Setup
 - git commit --allow-empty -m 'init'
 - git checkout -B main

 - git remote add torvalds
 - git fetch torvalds
 - git checkout ~torvalds/master~ v6.12
 - git checkout -B upstream

 - git checkout main
 # prepare for subtree merge - https://www.atlassian.com/git/tutorials/git-subtree "Do without subtree?"
 - git merge -s ours --no-commit --allow-unrelated-histories upstream
 # initialize subtree
 - git read-tree --prefix=upstream/ -u upstream
 - git commit

 # now, we can pull in upstream updates
 - git merge -X subtree=upstream/ v6.313
