#!/usr/local/bin/apl --script --
⍝ "Monkey on a typewriter"-style test.
⍝ This generates an infinite number of fake TECO macros, trying to crash
⍝ the parser.
⍝ The generated scripts are not entirely random, but follow the frequency
⍝ distribution calculated over the entire SciTECO "corpus", as generated by
⍝ monkey-parse.apl.
⍝ This requires a recent version of GNU APL and support for sandboxing
⍝ (currently only on FreeBSD).
5≤⍴⎕ARG →→ SciTECO←∊4↓⎕ARG ←→ SciTECO←"sciteco" ←←

⍝ Sorted character-to-frequency distribution,
Chars ← 8 63 7 86 38 9045 8594 8592 106 96 30 43 89 113 37 31 90 126 75 55 66 57 95 87 124 54 88 122 11 5 36 92 61 41 20 59 120 56 118 52 35 72 71 60 62 80 40 74 125 123 53 70 82 119 67 107 76 68 121 65 21 34 94 51 78 79 47 117 50 64 58 39 102 42 73 112 109 45 84 77 104 33 100 83 85 27 103 49 98 81 48 44 69 105 110 116 93 97 91 115 99 46 10 114 108 101 111 32
Freq ← 1 1 2 2 3 4 4 4 4 6 7 8 8 11 14 15 15 15 17 18 18 19 25 26 26 28 28 28 30 30 30 31 32 32 33 34 37 40 40 41 41 44 46 47 48 51 52 59 60 62 63 63 74 74 78 80 82 89 90 93 95 107 107 113 115 116 118 123 126 127 128 133 134 142 146 146 158 163 168 173 173 176 180 198 200 210 224 233 257 264 295 319 348 402 411 476 478 480 481 494 576 672 726 751 756 860 1033 2664

CumFreq ← +\Freq
Sum ← +/Freq

⍝ Random seed
⊣⎕RL ← +/⎕TS

⍝ Codepoints ← GenTECO Len
GenTECO ← {Chars[{(CumFreq ≥ ⍵) ⍳ 1}¨⍵ ? Sum]~0}
GenArg  ← {(⍵ ? $3000)~0 27}

⍝ Escape as shell argument
Escape ← {"'",(∊{⍺,"'\\''",⍵}/(⍵≠"'") ⊂ ⍵),"'"}

⍝ File WriteFile Contents
WriteFile ← {⊣⍵ ⎕FIO['fwrite_UNI'] Hnd ← "w" ⎕FIO['fopen'] ⍺ ⋄ ⊣⎕FIO['fclose'] Hnd; Hnd}

∇ Sig←Arg Exec Macro; Hnd; Data; Stdout
  ⍝ FIXME: We cannot currently mung files in --sandbox mode.
  ⍝ To support that, we would effectively have to add a test compilation unit,
  ⍝ in which case we could just generate the test macro in C code as well.
  Macro ← "EI",Arg,(⎕UCS 27),"J ",Macro
  ⍝ We can generate loops by accident, but they should all be interruptible with SIGINT.
  ⍝ If the process needs to be killed, this is also considered a fatal issue.
  Hnd ← ⎕FIO['popen'] "timeout --signal SIGINT --kill-after 2 2 ",SciTECO," --sandbox --eval ",(Escape Macro)," 2>&1"

  Stdout ← ""
ReadStdout:
  Stdout ← Stdout,Data ← ⎕UCS (⎕FIO['fread'] Hnd)
→(0≤↑Data)/ReadStdout

Cleanup:
  ⍝⎕ ← Stdout
  ⍝ Extract WTERMSIG()
  ⍝ FIXME: This may work only on FreeBSD
  Sig ← $7F ⊤∧ (⎕FIO['pclose'] Hnd)
  Sig≠0 →→ "monkey-bug.tec" WriteFile Macro ⋄ ⍞ ← Stdout ←←
∇

∇ RunBatch; Sig
Loop:
  Sig ← (⎕UCS GenArg ?100) Exec (⎕UCS GenTECO ?100)
→(Sig=0)/Loop
  ⎕ ← "Script in monkey-bug.tec terminated with signal: " Sig
∇

RunBatch

)OFF
