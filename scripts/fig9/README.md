# Figure 9

1. Set up DLB drivers on the server
```
cd scripts/common/
sudo ./setup_libdlb_dlb2.sh
```

2. Run experiment on the client
```
python3 run.py
```

3. Process results and draw figures
```
python3 plot.py
```