This value: $(x=`expr 1 + 2`; echo $x)

Should persist: $(echo $x)

New value: $(x=`expr $x + $x`; echo $x)

Should persist: $(echo $x)
