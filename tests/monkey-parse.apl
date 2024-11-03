#!/usr/local/bin/apl --script --
⍝ Calculate frequency distribution of the characters/glyphs in a given number of input files.
⍝ This will be the input for monkey-test.apl.
Files ← 4↓⎕ARG
Data ← 19 ⎕CR ∊⎕FIO['read_file']¨Files

∇ Dist←GetDist Data; i; C
  ⍝Dist ← 38 ⌷CR ↑⍴Data ⍝ associative array
  Dist.X ← 0
  i ← 0
Loop:
  C ← ,Data[i←i+1]
  ~∨/∊C=Dist[;1] →→ Dist[C] ← 0 ←←
  Dist[C] ← Dist[C] + 1
→(i<⍴Data)/Loop
∇

Dist ← GetDist Data
Freq ← {Dist[⍵]}¨Dist[;1]
⎕ ← "Chars ←", ⎕UCS ∊Dist[;1][Sort←⍋Freq] ⍝ character codepoints
⎕ ← "Freq ←", Freq[Sort] ⍝ character frequencies

)OFF
