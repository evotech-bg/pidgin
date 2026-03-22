// Package pidgin implements a Caddy HTTP middleware that provides
// Pidgin JSON compression via libpidgin (cgo).
//
// Caddyfile syntax:
//
//	pidgin {
//	    learn_samples   30
//	    min_size        256
//	    evolve_interval 500
//	}
//
// Or shorthand:
//
//	pidgin
//
// The module:
//   - Observes JSON responses to auto-learn per-endpoint profiles.
//   - Compresses when client sends Accept-Encoding: pidgin.
//   - Sets Content-Encoding: pidgin on compressed responses.
//   - Serves profiles at /.well-known/pidgin/<endpoint>.
package pidgin

/*
#cgo LDFLAGS: -lpidgin -lbrotlienc -lbrotlidec
#include <stdlib.h>
#include "pidgin.h"
*/
import "C"

import (
	"bytes"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"unsafe"

	"github.com/caddyserver/caddy/v2"
	"github.com/caddyserver/caddy/v2/caddyconfig/caddyfile"
	"github.com/caddyserver/caddy/v2/caddyconfig/httpcaddyfile"
	"github.com/caddyserver/caddy/v2/modules/caddyhttp"
	"go.uber.org/zap"
)

func init() {
	caddy.RegisterModule(Middleware{})
	httpcaddyfile.RegisterHandlerDirective("pidgin", parseCaddyfile)
}

// ------------------------------------------------------------------ //
// Module definition                                                   //
// ------------------------------------------------------------------ //

// Middleware implements Pidgin JSON compression as a Caddy middleware.
type Middleware struct {
	LearnSamples   int `json:"learn_samples,omitempty"`
	MinSize        int `json:"min_size,omitempty"`
	EvolveInterval int `json:"evolve_interval,omitempty"`

	logger *zap.Logger
	engine *C.pidgin_engine_t
	mu     sync.Mutex
}

// CaddyModule returns the Caddy module information.
func (Middleware) CaddyModule() caddy.ModuleInfo {
	return caddy.ModuleInfo{
		ID:  "http.handlers.pidgin",
		New: func() caddy.Module { return new(Middleware) },
	}
}

// Provision sets up the module.
func (m *Middleware) Provision(ctx caddy.Context) error {
	m.logger = ctx.Logger()

	if m.LearnSamples <= 0 {
		m.LearnSamples = 30
	}
	if m.MinSize <= 0 {
		m.MinSize = 256
	}
	if m.EvolveInterval <= 0 {
		m.EvolveInterval = 500
	}

	m.engine = C.pidgin_engine_create(
		C.int(m.LearnSamples),
		C.int(m.EvolveInterval),
	)
	if m.engine == nil {
		return fmt.Errorf("pidgin: failed to create engine")
	}

	ver := C.pidgin_version()
	m.logger.Info("pidgin module initialized",
		zap.String("version", C.GoString(ver)),
		zap.Int("learn_samples", m.LearnSamples),
		zap.Int("min_size", m.MinSize),
		zap.Int("evolve_interval", m.EvolveInterval),
	)

	return nil
}

// Validate ensures the module configuration is valid.
func (m *Middleware) Validate() error {
	if m.LearnSamples < 1 {
		return fmt.Errorf("pidgin: learn_samples must be >= 1")
	}
	if m.MinSize < 0 {
		return fmt.Errorf("pidgin: min_size must be >= 0")
	}
	return nil
}

// Cleanup releases the engine when the module is destroyed.
func (m *Middleware) Cleanup() error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.engine != nil {
		C.pidgin_engine_destroy(m.engine)
		m.engine = nil
	}
	return nil
}

// ------------------------------------------------------------------ //
// HTTP handler                                                        //
// ------------------------------------------------------------------ //

// ServeHTTP implements caddyhttp.MiddlewareHandler.
func (m *Middleware) ServeHTTP(w http.ResponseWriter, r *http.Request, next caddyhttp.Handler) error {
	// Handle /.well-known/pidgin/ requests — intercept before proxying
	if strings.HasPrefix(r.URL.Path, "/.well-known/pidgin/") {
		return m.serveProfile(w, r)
	}

	// Use Caddy's built-in ResponseRecorder to buffer the full response
	rec := caddyhttp.NewResponseRecorder(w, &bytes.Buffer{}, func(status int, header http.Header) bool {
		// Buffer all responses — we decide later whether to compress
		return true
	})

	err := next.ServeHTTP(rec, r)
	if err != nil {
		return err
	}

	// Now process the buffered response
	return m.processResponse(w, r, rec)
}

// processResponse examines the buffered response and decides whether to
// observe it for learning and/or compress it with pidgin.
func (m *Middleware) processResponse(w http.ResponseWriter, r *http.Request, rec caddyhttp.ResponseRecorder) error {
	body := rec.Buffer().Bytes()
	status := rec.Status()
	header := rec.Header()

	ct := header.Get("Content-Type")
	isJSON := strings.Contains(ct, "json")

	// Pass through if not JSON, not 2xx, or too small
	if !isJSON || status < 200 || status >= 300 || len(body) < m.MinSize {
		// Copy headers and write through
		for k, vals := range header {
			for _, v := range vals {
				w.Header().Add(k, v)
			}
		}
		w.WriteHeader(status)
		_, writeErr := w.Write(body)
		return writeErr
	}

	// Normalize endpoint path
	endpoint := r.URL.Path
	if len(endpoint) > 1 && strings.HasSuffix(endpoint, "/") {
		endpoint = endpoint[:len(endpoint)-1]
	}

	cEndpoint := C.CString(endpoint)
	cBody := C.CString(string(body))
	defer C.free(unsafe.Pointer(cEndpoint))
	defer C.free(unsafe.Pointer(cBody))

	// Always observe for learning
	m.mu.Lock()
	C.pidgin_engine_observe(m.engine, cEndpoint, cBody)

	// Check if client accepts pidgin encoding and engine is ready
	acceptsPidgin := strings.Contains(r.Header.Get("Accept-Encoding"), "pidgin")

	if acceptsPidgin && C.pidgin_engine_ready(m.engine, cEndpoint) == 1 {
		compressed := C.pidgin_engine_compress(m.engine, cEndpoint, cBody)
		m.mu.Unlock()

		cLen := int(compressed.len)
		if compressed.data != nil && cLen > 0 && cLen < len(body) {
			compData := C.GoBytes(unsafe.Pointer(compressed.data), C.int(compressed.len))
			ratio := float64(cLen) / float64(len(body)) * 100.0
			C.pidgin_buf_free(&compressed)

			// Copy upstream headers, then override encoding-related ones
			for k, vals := range header {
				for _, v := range vals {
					w.Header().Add(k, v)
				}
			}
			w.Header().Set("Content-Encoding", "pidgin")
			w.Header().Set("Content-Length", strconv.Itoa(len(compData)))
			w.Header().Set("X-Pidgin-Ratio", fmt.Sprintf("%.1f%%", ratio))
			w.Header().Set("Vary", "Accept-Encoding")
			w.WriteHeader(status)

			m.logger.Info("pidgin compressed",
				zap.String("endpoint", endpoint),
				zap.Int("original", len(body)),
				zap.Int("compressed", len(compData)),
				zap.String("ratio", fmt.Sprintf("%.1f%%", ratio)),
			)

			_, writeErr := w.Write(compData)
			return writeErr
		}

		if compressed.data != nil {
			C.pidgin_buf_free(&compressed)
		}
	} else {
		m.mu.Unlock()
	}

	// Pass through original body
	for k, vals := range header {
		for _, v := range vals {
			w.Header().Add(k, v)
		}
	}
	w.WriteHeader(status)
	_, writeErr := w.Write(body)
	return writeErr
}

// ------------------------------------------------------------------ //
// Profile serving                                                     //
// ------------------------------------------------------------------ //

func (m *Middleware) serveProfile(w http.ResponseWriter, r *http.Request) error {
	prefix := "/.well-known/pidgin"
	remainder := r.URL.Path[len(prefix):]
	if remainder == "" || remainder == "/" {
		http.Error(w, `{"error":"endpoint required"}`, http.StatusBadRequest)
		return nil
	}

	endpoint := remainder
	if !strings.HasPrefix(endpoint, "/") {
		endpoint = "/" + endpoint
	}

	cEndpoint := C.CString(endpoint)
	defer C.free(unsafe.Pointer(cEndpoint))

	m.mu.Lock()
	profilePtr := C.pidgin_engine_profile(m.engine, cEndpoint)
	var etag string
	if profilePtr != nil {
		etagPtr := C.pidgin_engine_etag(m.engine, cEndpoint)
		if etagPtr != nil {
			etag = C.GoString(etagPtr)
		}
	}
	m.mu.Unlock()

	if profilePtr == nil {
		http.Error(w, `{"error":"no profile for endpoint"}`, http.StatusNotFound)
		return nil
	}

	profileJSON := C.GoString(profilePtr)
	C.pidgin_free(unsafe.Pointer(profilePtr))

	// Check If-None-Match for caching
	if etag != "" && r.Header.Get("If-None-Match") == etag {
		w.WriteHeader(http.StatusNotModified)
		return nil
	}

	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "public, max-age=60")
	if etag != "" {
		w.Header().Set("ETag", etag)
	}

	w.WriteHeader(http.StatusOK)
	_, writeErr := w.Write([]byte(profileJSON))
	return writeErr
}

// ------------------------------------------------------------------ //
// Caddyfile parsing                                                   //
// ------------------------------------------------------------------ //

// UnmarshalCaddyfile parses the Caddyfile tokens.
func (m *Middleware) UnmarshalCaddyfile(d *caddyfile.Dispenser) error {
	for d.Next() {
		for d.NextBlock(0) {
			switch d.Val() {
			case "learn_samples":
				if !d.NextArg() {
					return d.ArgErr()
				}
				val, err := strconv.Atoi(d.Val())
				if err != nil {
					return d.Errf("invalid learn_samples: %v", err)
				}
				m.LearnSamples = val

			case "min_size":
				if !d.NextArg() {
					return d.ArgErr()
				}
				val, err := strconv.Atoi(d.Val())
				if err != nil {
					return d.Errf("invalid min_size: %v", err)
				}
				m.MinSize = val

			case "evolve_interval":
				if !d.NextArg() {
					return d.ArgErr()
				}
				val, err := strconv.Atoi(d.Val())
				if err != nil {
					return d.Errf("invalid evolve_interval: %v", err)
				}
				m.EvolveInterval = val

			default:
				return d.Errf("unknown pidgin directive: %s", d.Val())
			}
		}
	}
	return nil
}

func parseCaddyfile(h httpcaddyfile.Helper) (caddyhttp.MiddlewareHandler, error) {
	var m Middleware
	err := m.UnmarshalCaddyfile(h.Dispenser)
	return &m, err
}

// ------------------------------------------------------------------ //
// Interface assertions                                                //
// ------------------------------------------------------------------ //

var (
	_ caddy.Module                = (*Middleware)(nil)
	_ caddy.Provisioner           = (*Middleware)(nil)
	_ caddy.Validator             = (*Middleware)(nil)
	_ caddy.CleanerUpper          = (*Middleware)(nil)
	_ caddyhttp.MiddlewareHandler = (*Middleware)(nil)
	_ caddyfile.Unmarshaler       = (*Middleware)(nil)
)
