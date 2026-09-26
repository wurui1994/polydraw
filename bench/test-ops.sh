#!/usr/bin/env bash
# bench/test-ops.sh —— **按指令族**的判据（给 JIT 用）。
#
# 与 `bench/test-a64.sh` 的分工：那一份是"语言跑不跑得起来"（16 行，带期望值）；
# 这一份是"JIT 的每一族指令算得对不对" —— 只印 `脚本<TAB>值`，判据是**三条路互相
# 逐字节相同**：arm64 的 JIT / x86-64 的解释器（PD_JIT=0）/ x86-64 的 JIT。
# 于是不用为每一行手算期望值（那反而容易把"两边都错"当成对），
# 而是拿已经判过的 arm64 那条腿当尺子。
#
#   BIN=… bash bench/test-ops.sh > /tmp/a.txt
#   PD_JIT=0 BIN=… bash bench/test-ops.sh > /tmp/b.txt   # 同一个二进制关掉 JIT
#   diff /tmp/a.txt /tmp/b.txt
#
# 每一族都摆了**NaN 那一格**（`sqrt(-1)`）—— 比较与 min/max 的无序语义是最容易
# 两边不一样的地方（arm64 是 FCMP+cond、x86 是 ucomisd+setcc，NaN 时标志位的含义
# 正好相反，所以 x86 那一份要"把操作数反过来比"）。
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=${BIN:-bench/out/eval_a64}
[ -x "$BIN" ] || { echo "没有 $BIN"; exit 1; }

run () { # run 脚本 [实参...]
	local src=$1; shift
	local got
	got=$(timeout 10 "$BIN" "$src" "$@" 2>/dev/null | sed -n 's/^value: *\([^ ]*\).*/\1/p' | head -1)
	printf '%s\t%s\n' "$src" "${got:-崩了}"
}

# 一元
for s in 'floor(3.7)' 'floor(-3.7)' 'ceil(3.2)' 'ceil(-3.2)' 'abs(-5)' 'abs(5)' \
	'-(-7)' 'sqrt(2)' 'sgn(-3)' 'sgn(0)' 'sgn(4)' 'unit(-3)' 'unit(0)' 'unit(4)' \
	'sin(1)' 'cos(1)' 'tan(1)' 'asin(.5)' 'acos(.5)' 'atan(2)' 'exp(1)' 'log(10)' 'fact(5)'; do
	run "$s"
done
# 二元
for s in '7%3' '-7%3' '7.5%2' '7%-3' 'pow(2,10)' 'pow(2,.5)' 'atan2(1,1)' 'atan2(-1,2)' \
	'min(2,5)' 'max(2,5)' 'min(-1,-2)' 'max(-1,-2)' 'min(0,-0)' \
	'1/3' '2*3.5' '10-11' '1+2'; do
	run "$s"
done
# NaN 那一格（无序语义）。**注意**：脚本第一个字符是 `(` 的话整句会被当成
# **参数表**（`(x){…}` 那个形式），所以这几行一律以 `0+` 开头。
for s in 'min(sqrt(-1),1)' 'min(1,sqrt(-1))' 'max(sqrt(-1),1)' 'max(1,sqrt(-1))' \
	'0+(sqrt(-1)<1)*10' '0+(sqrt(-1)<=1)*10' '0+(sqrt(-1)>1)*10' '0+(sqrt(-1)>=1)*10' \
	'0+(sqrt(-1)==1)*10' '0+(sqrt(-1)!=1)*10' '0+(sqrt(-1)&&1)*10' '0+(sqrt(-1)||0)*10'; do
	run "$s"
done
# 比较与逻辑（当数用 —— 这一格错过一次：cset 之后转成了 float）
for s in '0+(2<3)*10' '0+(3<3)*10' '0+(3<=3)*10' '0+(4<=3)*10' '0+(2>3)*10' '0+(4>3)*10' \
	'0+(3>=4)*10' '0+(3>=3)*10' '0+(2==2)*10' '0+(2==3)*10' '0+(2!=2)*10' '0+(2!=3)*10' \
	'0+(1&&0)+5' '0+(1&&2)+5' '0+(0||3)*2' '0+(0||0)*2'; do
	run "$s"
done
# 数组那两族（PEEK/POKE 全套赋值算子 + 越界那一夹）
run '(){static a[4];a[0]=3;a[1]=4;sqrt(a[0]*a[0]+a[1]*a[1])}'
run '(){static a[4];a[0]=10;a[0]+=5;a[0]}'
run '(){static a[4];a[0]=10;a[0]-=5;a[0]}'
run '(){static a[4];a[0]=10;a[0]*=5;a[0]}'
run '(){static a[4];a[0]=10;a[0]/=4;a[0]}'
run '(){static a[4];a[0]=10;a[0]%=3;a[0]}'
run '(){static a[4];a[0]=1;a[1]=2;a[2]=3;a[3]=4;i=2;a[i]}'
# 越界那一夹（下标得是**编译期折不掉**的量 —— 写死的 `a[9]` 与 `i=9;a[i]` 都会被
# 前端当场判成 "array index out of bounds"，所以从实参递进来）
run '(x){static a[4];a[x]=5;a[1]}' 9
run '(x){static a[3];a[x]=7;a[0]+a[1]+a[2]}' 5
# 控制流与函数
run '(x){s=0;for(i=0;i<x;i++)s=s+i*i;s}' 20
run '(x){s=0;for(i=0;i<x;i++){if(i%2)s+=i;else s-=i;}s}' 20
run '(x){f(x)}f(n){if(n<=1)return(1);n*f(n-1)}' 10
run '(x){fib(x)}fib(n){if(n<2)return(n);fib(n-1)+fib(n-2)}' 15
run '(x){g(x)+h(x)}g(a){a*2}h(a){a*3}' 7
run '(x){y=0;for(;y<x;)y++;y}' 100
