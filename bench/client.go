package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

func cpuTicks(pid string) (uint64, error) {
	b, err := os.ReadFile("/proc/" + pid + "/stat")
	if err != nil {
		return 0, err
	}
	s := string(b)
	i := strings.LastIndex(s, ")")
	if i < 0 {
		return 0, errors.New("invalid process stat")
	}
	fields := strings.Fields(s[i+1:])
	if len(fields) < 13 {
		return 0, errors.New("short process stat")
	}
	user, err := strconv.ParseUint(fields[11], 10, 64)
	if err != nil {
		return 0, err
	}
	system, err := strconv.ParseUint(fields[12], 10, 64)
	return user + system, err
}

func main() {
	if len(os.Args) != 4 {
		fmt.Fprintln(os.Stderr, "usage: client URL NGINX_PID CLOCK_TICKS_PER_SECOND")
		os.Exit(2)
	}
	hz, err := strconv.ParseFloat(os.Args[3], 64)
	if err != nil || hz <= 0 {
		fmt.Fprintln(os.Stderr, "invalid clock tick rate")
		os.Exit(2)
	}
	url, pid := os.Args[1], os.Args[2]
	var count atomic.Uint64
	var measured atomic.Bool
	var stop atomic.Bool
	errorsCh := make(chan error, 4)
	var wg sync.WaitGroup

	for i := 0; i < 4; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			transport := &http.Transport{
				MaxIdleConnsPerHost: 1,
				MaxConnsPerHost:     1,
				DisableCompression:  true,
			}
			defer transport.CloseIdleConnections()
			client := &http.Client{Transport: transport, Timeout: 10 * time.Second}
			for !stop.Load() {
				record := measured.Load()
				req, err := http.NewRequestWithContext(context.Background(), "GET", url, nil)
				if err != nil {
					errorsCh <- err
					return
				}
				req.Header.Set("Accept-Encoding", "zstd")
				resp, err := client.Do(req)
				if err != nil {
					errorsCh <- err
					return
				}
				_, err = io.Copy(io.Discard, resp.Body)
				closeErr := resp.Body.Close()
				if err != nil || closeErr != nil || resp.StatusCode != 200 ||
					resp.Header.Get("Content-Encoding") != "zstd" {
					errorsCh <- fmt.Errorf("invalid response: status=%d encoding=%q body=%v close=%v",
						resp.StatusCode, resp.Header.Get("Content-Encoding"), err, closeErr)
					return
				}
				if record {
					count.Add(1)
				}
			}
		}()
	}

	time.Sleep(600 * time.Millisecond)
	before, err := cpuTicks(pid)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	start := time.Now()
	measured.Store(true)
	time.Sleep(4 * time.Second)
	stop.Store(true)
	wg.Wait()
	elapsed := time.Since(start).Seconds()
	after, err := cpuTicks(pid)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	close(errorsCh)
	for err := range errorsCh {
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	}
	n := count.Load()
	if n == 0 {
		fmt.Fprintln(os.Stderr, "no completed requests")
		os.Exit(1)
	}
	result := map[string]float64{
		"requests":           float64(n),
		"seconds":            elapsed,
		"rps":                float64(n) / elapsed,
		"cpu_sec":            float64(after-before) / hz,
		"cpu_us_per_request": float64(after-before) / hz / float64(n) * 1e6,
	}
	if err := json.NewEncoder(os.Stdout).Encode(result); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}