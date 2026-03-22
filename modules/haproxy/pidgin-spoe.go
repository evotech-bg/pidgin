// pidgin-spoe.go — HAProxy SPOE agent for Pidgin JSON compression.
//
// This agent listens on a Unix socket (or TCP) and communicates with
// HAProxy via the SPOP protocol. It receives JSON response bodies,
// runs them through the pidgin engine (via cgo + libpidgin), and
// returns compressed data.
//
// Architecture:
//   HAProxy receives response from backend
//   -> SPOE filter sends body to this agent via SPOP
//   -> Agent runs pidgin_engine_observe + pidgin_engine_compress
//   -> Returns compressed body (or empty if not ready/not smaller)
//   -> HAProxy replaces the response body
//
// Build:
//   CGO_ENABLED=1 go build -o pidgin-spoe pidgin-spoe.go

package main

/*
#cgo LDFLAGS: -lpidgin -lbrotlienc -lbrotlidec
#include <stdlib.h>
#include "pidgin.h"
*/
import "C"

import (
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"unsafe"
)

// ================================================================
// Pidgin engine wrapper (cgo)
// ================================================================

type pidginEngine struct {
	mu     sync.Mutex
	engine *C.pidgin_engine_t
}

func newPidginEngine(learnSamples, evolveInterval int) *pidginEngine {
	eng := C.pidgin_engine_create(C.int(learnSamples), C.int(evolveInterval))
	if eng == nil {
		log.Fatal("pidgin: failed to create engine")
	}
	ver := C.GoString(C.pidgin_version())
	log.Printf("pidgin engine v%s initialized (learn=%d, evolve=%d)", ver, learnSamples, evolveInterval)
	return &pidginEngine{engine: eng}
}

func (p *pidginEngine) observe(endpoint, jsonBody string) {
	cEndpoint := C.CString(endpoint)
	cBody := C.CString(jsonBody)
	defer C.free(unsafe.Pointer(cEndpoint))
	defer C.free(unsafe.Pointer(cBody))

	p.mu.Lock()
	C.pidgin_engine_observe(p.engine, cEndpoint, cBody)
	p.mu.Unlock()
}

func (p *pidginEngine) ready(endpoint string) bool {
	cEndpoint := C.CString(endpoint)
	defer C.free(unsafe.Pointer(cEndpoint))

	p.mu.Lock()
	r := C.pidgin_engine_ready(p.engine, cEndpoint)
	p.mu.Unlock()
	return r == 1
}

func (p *pidginEngine) compress(endpoint, jsonBody string) []byte {
	cEndpoint := C.CString(endpoint)
	cBody := C.CString(jsonBody)
	defer C.free(unsafe.Pointer(cEndpoint))
	defer C.free(unsafe.Pointer(cBody))

	p.mu.Lock()
	buf := C.pidgin_engine_compress(p.engine, cEndpoint, cBody)
	p.mu.Unlock()

	if buf.data == nil || buf.len == 0 {
		return nil
	}

	result := C.GoBytes(unsafe.Pointer(buf.data), C.int(buf.len))
	C.pidgin_buf_free(&buf)
	return result
}

func (p *pidginEngine) profile(endpoint string) string {
	cEndpoint := C.CString(endpoint)
	defer C.free(unsafe.Pointer(cEndpoint))

	p.mu.Lock()
	ptr := C.pidgin_engine_profile(p.engine, cEndpoint)
	p.mu.Unlock()

	if ptr == nil {
		return ""
	}

	result := C.GoString(ptr)
	C.pidgin_free(unsafe.Pointer(ptr))
	return result
}

func (p *pidginEngine) etag(endpoint string) string {
	cEndpoint := C.CString(endpoint)
	defer C.free(unsafe.Pointer(cEndpoint))

	p.mu.Lock()
	ptr := C.pidgin_engine_etag(p.engine, cEndpoint)
	p.mu.Unlock()

	if ptr == nil {
		return ""
	}
	return C.GoString(ptr)
}

func (p *pidginEngine) destroy() {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.engine != nil {
		C.pidgin_engine_destroy(p.engine)
		p.engine = nil
	}
}

// ================================================================
// SPOP frame types and constants
// ================================================================

// SPOP frame types
const (
	frameTypeHaproxyHello   = 1
	frameTypeHaproxyDisconn = 2
	frameTypeHaproxyNotify  = 3
	frameTypeAgentHello     = 101
	frameTypeAgentDisconn   = 102
	frameTypeAgentAck       = 103
)

// SPOP data types
const (
	spopTypeNull   = 0
	spopTypeBool   = 1
	spopTypeInt32  = 2
	spopTypeUint32 = 3
	spopTypeInt64  = 4
	spopTypeUint64 = 5
	spopTypeIPv4   = 6
	spopTypeIPv6   = 7
	spopTypeStr    = 8
	spopTypeBin    = 9
)

// ================================================================
// SPOP protocol helpers
// ================================================================

func readVarInt(data []byte, offset int) (uint64, int) {
	if offset >= len(data) {
		return 0, offset
	}

	val := uint64(data[offset])
	offset++

	if val < 240 {
		return val, offset
	}

	// Multi-byte encoding
	val = val - 240
	shift := uint(0)
	for offset < len(data) {
		b := uint64(data[offset])
		offset++
		val += (b & 0x7F) << (4 + shift)
		if b < 128 {
			break
		}
		shift += 7
	}
	return val + 240, offset
}

func writeVarInt(buf []byte, val uint64) int {
	if val < 240 {
		buf[0] = byte(val)
		return 1
	}

	buf[0] = byte(240 + (val-240)&0x0F)
	val = (val - 240) >> 4
	n := 1
	for val > 0 {
		if val >= 128 {
			buf[n] = byte(128 | (val & 0x7F))
		} else {
			buf[n] = byte(val & 0x7F)
		}
		val >>= 7
		n++
	}
	return n
}

func writeVarIntToSlice(val uint64) []byte {
	buf := make([]byte, 10)
	n := writeVarInt(buf, val)
	return buf[:n]
}

func readString(data []byte, offset int) (string, int) {
	length, offset := readVarInt(data, offset)
	if offset+int(length) > len(data) {
		return "", offset
	}
	s := string(data[offset : offset+int(length)])
	return s, offset + int(length)
}

func readBinary(data []byte, offset int) ([]byte, int) {
	length, offset := readVarInt(data, offset)
	if offset+int(length) > len(data) {
		return nil, offset
	}
	b := make([]byte, length)
	copy(b, data[offset:offset+int(length)])
	return b, offset + int(length)
}

// ================================================================
// SPOP frame reading/writing
// ================================================================

func readFrame(conn net.Conn) ([]byte, error) {
	// Read 4-byte frame length
	lenBuf := make([]byte, 4)
	n, err := conn.Read(lenBuf)
	if err != nil {
		return nil, err
	}
	if n < 4 {
		return nil, fmt.Errorf("short read for frame length")
	}

	frameLen := uint32(lenBuf[0])<<24 | uint32(lenBuf[1])<<16 | uint32(lenBuf[2])<<8 | uint32(lenBuf[3])
	if frameLen == 0 {
		return nil, fmt.Errorf("zero-length frame")
	}
	if frameLen > 1<<20 { // 1MB max
		return nil, fmt.Errorf("frame too large: %d", frameLen)
	}

	frame := make([]byte, frameLen)
	total := 0
	for total < int(frameLen) {
		n, err := conn.Read(frame[total:])
		if err != nil {
			return nil, err
		}
		total += n
	}

	return frame, nil
}

func writeFrame(conn net.Conn, frame []byte) error {
	lenBuf := make([]byte, 4)
	fLen := uint32(len(frame))
	lenBuf[0] = byte(fLen >> 24)
	lenBuf[1] = byte(fLen >> 16)
	lenBuf[2] = byte(fLen >> 8)
	lenBuf[3] = byte(fLen)

	if _, err := conn.Write(lenBuf); err != nil {
		return err
	}
	_, err := conn.Write(frame)
	return err
}

// ================================================================
// SPOP message handling
// ================================================================

func buildAgentHello() []byte {
	var buf []byte

	// Frame type: AGENT-HELLO
	buf = append(buf, byte(frameTypeAgentHello))

	// Flags: FIN=1
	buf = append(buf, 0x01, 0x00, 0x00, 0x00)

	// Stream-id = 0, Frame-id = 0
	buf = append(buf, 0x00) // stream-id varint
	buf = append(buf, 0x00) // frame-id varint

	// KV: version
	key := "version"
	buf = append(buf, writeVarIntToSlice(uint64(len(key)))...)
	buf = append(buf, []byte(key)...)
	buf = append(buf, byte(spopTypeStr))
	ver := "2.0"
	buf = append(buf, writeVarIntToSlice(uint64(len(ver)))...)
	buf = append(buf, []byte(ver)...)

	// KV: max-frame-size
	key2 := "max-frame-size"
	buf = append(buf, writeVarIntToSlice(uint64(len(key2)))...)
	buf = append(buf, []byte(key2)...)
	buf = append(buf, byte(spopTypeUint32))
	buf = append(buf, writeVarIntToSlice(65535)...)

	// KV: capabilities
	key3 := "capabilities"
	buf = append(buf, writeVarIntToSlice(uint64(len(key3)))...)
	buf = append(buf, []byte(key3)...)
	buf = append(buf, byte(spopTypeStr))
	caps := ""
	buf = append(buf, writeVarIntToSlice(uint64(len(caps)))...)
	buf = append(buf, []byte(caps)...)

	return buf
}

func buildAck(streamID, frameID uint64, actions []byte) []byte {
	var buf []byte

	// Frame type: AGENT-ACK
	buf = append(buf, byte(frameTypeAgentAck))

	// Flags: FIN=1
	buf = append(buf, 0x01, 0x00, 0x00, 0x00)

	// Stream-id and Frame-id
	buf = append(buf, writeVarIntToSlice(streamID)...)
	buf = append(buf, writeVarIntToSlice(frameID)...)

	// Actions (KV pairs)
	buf = append(buf, actions...)

	return buf
}

func buildSetVarAction(scope byte, name string, dataType byte, value []byte) []byte {
	var buf []byte

	// Action type: set-var (1)
	buf = append(buf, 0x01)

	// Number of args: 3
	buf = append(buf, 0x03)

	// Arg 1: scope (byte)
	buf = append(buf, byte(spopTypeUint32))
	buf = append(buf, scope)

	// Arg 2: variable name (string)
	buf = append(buf, byte(spopTypeStr))
	buf = append(buf, writeVarIntToSlice(uint64(len(name)))...)
	buf = append(buf, []byte(name)...)

	// Arg 3: value
	buf = append(buf, dataType)
	buf = append(buf, value...)

	return buf
}

// ================================================================
// SPOE connection handler
// ================================================================

func handleConnection(conn net.Conn, eng *pidginEngine, minSize int) {
	defer conn.Close()

	for {
		frame, err := readFrame(conn)
		if err != nil {
			return
		}

		if len(frame) < 5 {
			continue
		}

		frameType := frame[0]
		// flags := binary.BigEndian.Uint32(frame[1:5])
		offset := 5

		// Read stream-id and frame-id
		streamID, offset := readVarInt(frame, offset)
		frameID, offset := readVarInt(frame, offset)

		switch frameType {
		case frameTypeHaproxyHello:
			// Respond with AGENT-HELLO
			hello := buildAgentHello()
			if err := writeFrame(conn, hello); err != nil {
				return
			}

		case frameTypeHaproxyDisconn:
			return

		case frameTypeHaproxyNotify:
			// Parse messages
			actions := handleNotify(frame[offset:], eng, minSize)
			ack := buildAck(streamID, frameID, actions)
			if err := writeFrame(conn, ack); err != nil {
				return
			}
		}
	}
}

func handleNotify(data []byte, eng *pidginEngine, minSize int) []byte {
	offset := 0
	var allActions []byte

	for offset < len(data) {
		// Message name
		msgName, newOffset := readString(data, offset)
		offset = newOffset
		if msgName == "" {
			break
		}

		// Number of args
		nbArgs, newOffset := readVarInt(data, offset)
		offset = newOffset

		// Parse args
		args := make(map[string]interface{})
		for i := uint64(0); i < nbArgs; i++ {
			argName, newOffset := readString(data, offset)
			offset = newOffset

			if offset >= len(data) {
				break
			}

			argType := data[offset]
			offset++

			switch argType {
			case spopTypeStr:
				val, newOffset := readString(data, offset)
				offset = newOffset
				args[argName] = val

			case spopTypeBin:
				val, newOffset := readBinary(data, offset)
				offset = newOffset
				args[argName] = val

			case spopTypeInt32, spopTypeInt64:
				val, newOffset := readVarInt(data, offset)
				offset = newOffset
				args[argName] = int64(val)

			case spopTypeUint32, spopTypeUint64:
				val, newOffset := readVarInt(data, offset)
				offset = newOffset
				args[argName] = val

			case spopTypeBool:
				if offset < len(data) {
					args[argName] = data[offset] != 0
					offset++
				}

			default:
				// Skip unknown
				break
			}
		}

		// Process based on message name
		switch msgName {
		case "pidgin-request":
			actions := handlePidginRequest(args, eng, minSize)
			allActions = append(allActions, actions...)
		}
	}

	return allActions
}

func handlePidginRequest(args map[string]interface{}, eng *pidginEngine, minSize int) []byte {
	endpoint, _ := args["path"].(string)
	body, _ := args["body"].(string)
	if bodyBytes, ok := args["body"].([]byte); ok {
		body = string(bodyBytes)
	}
	acceptEncoding, _ := args["accept_encoding"].(string)
	contentType, _ := args["content_type"].(string)

	// Normalize endpoint
	if idx := strings.IndexByte(endpoint, '?'); idx >= 0 {
		endpoint = endpoint[:idx]
	}
	if len(endpoint) > 1 && strings.HasSuffix(endpoint, "/") {
		endpoint = endpoint[:len(endpoint)-1]
	}

	// Only process JSON
	if !strings.Contains(contentType, "json") || len(body) < minSize {
		return nil
	}

	// Always observe
	eng.observe(endpoint, body)

	// Compress if client wants pidgin
	acceptsPidgin := strings.Contains(acceptEncoding, "pidgin")
	if acceptsPidgin && eng.ready(endpoint) {
		compressed := eng.compress(endpoint, body)
		if compressed != nil && len(compressed) < len(body) {
			ratio := float64(len(compressed)) / float64(len(body)) * 100.0
			log.Printf("pidgin: %s %d -> %d (%.1f%%)", endpoint, len(body), len(compressed), ratio)

			// Build set-var actions to return compressed body
			var actions []byte

			// Set txn.pidgin_body (binary)
			binVal := append(writeVarIntToSlice(uint64(len(compressed))), compressed...)
			actions = append(actions, buildSetVarAction(0x02, "pidgin_body", spopTypeBin, binVal)...)

			// Set txn.pidgin_encoding = "pidgin" (string)
			encStr := "pidgin"
			strVal := append(writeVarIntToSlice(uint64(len(encStr))), []byte(encStr)...)
			actions = append(actions, buildSetVarAction(0x02, "pidgin_encoding", spopTypeStr, strVal)...)

			// Set txn.pidgin_ratio (string)
			ratioStr := fmt.Sprintf("%.1f%%", ratio)
			ratioVal := append(writeVarIntToSlice(uint64(len(ratioStr))), []byte(ratioStr)...)
			actions = append(actions, buildSetVarAction(0x02, "pidgin_ratio", spopTypeStr, ratioVal)...)

			return actions
		}
	}

	return nil
}

// ================================================================
// HTTP server for profile endpoint
// ================================================================

func startProfileServer(addr string, eng *pidginEngine) {
	mux := http.NewServeMux()

	mux.HandleFunc("/.well-known/pidgin/", func(w http.ResponseWriter, r *http.Request) {
		prefix := "/.well-known/pidgin"
		remainder := r.URL.Path[len(prefix):]
		if remainder == "" || remainder == "/" {
			http.Error(w, `{"error":"endpoint required"}`, http.StatusBadRequest)
			return
		}

		endpoint := remainder
		if !strings.HasPrefix(endpoint, "/") {
			endpoint = "/" + endpoint
		}

		profileJSON := eng.profile(endpoint)
		if profileJSON == "" {
			http.Error(w, `{"error":"no profile for endpoint"}`, http.StatusNotFound)
			return
		}

		etag := eng.etag(endpoint)
		if etag != "" && r.Header.Get("If-None-Match") == etag {
			w.WriteHeader(http.StatusNotModified)
			return
		}

		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "public, max-age=60")
		if etag != "" {
			w.Header().Set("ETag", etag)
		}
		w.Write([]byte(profileJSON))
	})

	mux.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]string{
			"status": "ok",
			"server": "pidgin-spoe",
		})
	})

	log.Printf("Profile HTTP server listening on %s", addr)
	if err := http.ListenAndServe(addr, mux); err != nil {
		log.Printf("Profile HTTP server error: %v", err)
	}
}

// ================================================================
// Main
// ================================================================

func main() {
	socketPath := flag.String("socket", "/var/run/pidgin-spoe.sock", "SPOE Unix socket path")
	tcpAddr := flag.String("tcp", "", "TCP address to listen on (e.g. :12345), used instead of socket if set")
	httpAddr := flag.String("http", ":8880", "HTTP address for profile endpoint")
	learnSamples := flag.Int("learn", 30, "Number of samples before building profile")
	evolveInterval := flag.Int("evolve", 500, "Requests between schema evolution checks")
	minSizeFlag := flag.Int("min-size", 256, "Minimum response size to consider")
	flag.Parse()

	eng := newPidginEngine(*learnSamples, *evolveInterval)
	defer eng.destroy()

	// Start profile HTTP server
	go startProfileServer(*httpAddr, eng)

	// Listen for SPOE connections
	var listener net.Listener
	var err error

	if *tcpAddr != "" {
		listener, err = net.Listen("tcp", *tcpAddr)
		if err != nil {
			log.Fatalf("Failed to listen on %s: %v", *tcpAddr, err)
		}
		log.Printf("SPOE agent listening on TCP %s", *tcpAddr)
	} else {
		// Remove stale socket
		os.Remove(*socketPath)

		listener, err = net.Listen("unix", *socketPath)
		if err != nil {
			log.Fatalf("Failed to listen on %s: %v", *socketPath, err)
		}
		// Allow HAProxy to connect
		os.Chmod(*socketPath, 0666)
		log.Printf("SPOE agent listening on %s", *socketPath)
	}

	// Handle graceful shutdown
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sigCh
		log.Println("Shutting down...")
		listener.Close()
		eng.destroy()
		os.Exit(0)
	}()

	for {
		conn, err := listener.Accept()
		if err != nil {
			log.Printf("Accept error: %v", err)
			continue
		}
		go handleConnection(conn, eng, *minSizeFlag)
	}
}
