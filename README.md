# Mini Shell

Mini shell project for "Programmation Système" in S5 (L3).

## What's this? 

This repository contains a very little Shell, which was a small project done during University (B.Sc).  
Only Evaluation.c was done by me. Other files were given and were not to be modified as the grading was automatic.

## What works

- Return codes
- Redirections
- Pipes
- Jobs
- Foregroud/Background 
- Some interrupts: Ctrl-C, Ctrl-Z

## What doesn't work 

A lot, it's a truly simple shell.
Some complex expressions with pipes and redirections can sometimes hang.

However, even if it doesn't have a lot of features, it does work enough to be useable.

## Internal Commands

```sh
cd [dir]
echo [$? | arg ...]
exit
hash [text]   
fg [name]
bg [name]
help
```
