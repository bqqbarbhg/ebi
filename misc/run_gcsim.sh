
for i in {0..23}
do
    python gcsim.py $i 24 > out_$i.txt 2>&1 &
done

