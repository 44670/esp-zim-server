package main

import (
	"log"
	"net/http"
	"os"
	"strconv"
	"strings"
)

var zimFile *os.File

func zimHandler(w http.ResponseWriter, r *http.Request) {
	q := r.URL.RawQuery
	arr := strings.Split(q, ",")
	offset, err := strconv.ParseInt(arr[0], 10, 64)
	if err != nil {
		return
	}
	length, err := strconv.ParseInt(arr[1], 10, 64)
	if err != nil {
		return
	}
	log.Println(offset, length)
	if length > 10*1024*1024 {
		return
	}
	buf := make([]byte, length)
	n, err := zimFile.ReadAt(buf, offset)
	if err != nil {
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Write(buf[:n])
}

func main() {
	var err error
	zimFile, err = os.Open("D:\\ZIM\\wikipedia_en_all_maxi_2021-12.zim")
	if err != nil {
		log.Fatal(err)
	}
	mux := http.NewServeMux()
	mux.Handle("/", http.FileServer(http.Dir("../www")))
	mux.HandleFunc("/zim", zimHandler)
	err = http.ListenAndServe(":3000", mux)
	if err != nil {
		log.Fatal(err)
	}
}
