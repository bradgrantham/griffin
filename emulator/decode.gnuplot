set terminal png size 800,200
set output 'signal.png'
set style line 1 lw 2 lc rgb '#0060ad'
set yrange [-0.2:1.4]
set xlabel 'Cycle'
set ylabel 'Level'
plot 'data.txt' using 2:3 with steps ls 1 notitle
