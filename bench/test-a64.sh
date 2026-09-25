#!/usr/bin/env bash
# bench/test-a64.sh —— arm64 这条腿的判据：每一行是"脚本 -> 期望值"。
#
# 为什么要自己攒一张表而不是跑 Ken 自带的 main：自带那个到例子 #4（两个函数指针）
# 就崩（原文 32 位假设，见 port/README.md 第 4 条），而且它不报对错、只印数。
# 这里的每一行都有期望值，错了就红。
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=${BIN:-bench/out/eval_a64}
[ -x "$BIN" ] || { echo "先跑 bench/build-a64.sh"; exit 1; }

pass=0; fail=0
chk () { # chk 期望 脚本 [实参...]
	local want=$1; shift
	local src=$1; shift
	local got
	got=$(timeout 10 "$BIN" "$src" "$@" 2>&1 | sed -n 's/^value: *\([^ ]*\).*/\1/p' | head -1)
	if [ "$got" = "$want" ]; then pass=$((pass+1)); printf '  ok   %-44s = %s\n' "$src" "$got"
	else fail=$((fail+1)); printf '  FAIL %-44s = %s（要 %s）\n' "$src" "${got:-崩了}" "$want"; fi
}

# 算术与优先级
chk 7      '1+2*3'
chk 1024   'pow(2,10)'
chk 3      'log(exp(3))'
chk -1     'cos(PI)'
# 变量、循环、赋值算子（注意这门语言的 `while` 体不收 `{}`，循环一律 `for`）
chk 10     '(x){y=0;for(;y<x;)y++;y}' 10
chk 45     '(x){s=0;for(i=0;i<x;i++)s=s+i;s}' 10
chk 45     '(x){s=0;for(i=0;i<x;i++){s=s+i;};s}' 10
chk 64     '(x){x*=x;x-=36;x}' 10
# 多函数脚本（原文那句 "1 script in memory" 的 hack 在这儿必崩）
chk 25     '(){g(5)}g(a){a*a}'
chk 50     '(x){g(x)+h(x)}g(a){a*2}h(a){a*3}' 10
# 递归与向后引用（要靠 pd_a64_refresh_ext 把 kcd 里那份 gevalext 抄本刷一遍）
chk 120    '(x){f(x)}f(n){if(n<=1)return(1);n*f(n-1)}' 5
chk 13     '(x){fib(x)}fib(n){if(n<2)return(n);fib(n-1)+fib(n-2)}' 7
# 静态变量（走 gstatmem 那条路 —— kasm87free 要把它一起放掉）
chk 3      '(){static s;s++;s++;s++;s}'
# 内建函数
chk 5      'sqrt(9+16)'
chk 6      'fact(3)'
chk 2      'max(min(2,5),1)'

echo
echo "$pass passed, $fail failed（arm64 osx：COMPILE=0 + port/a64 的 thunk）"
[ "$fail" = 0 ]
