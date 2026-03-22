// Package traefik_plugin_pidgin implements Pidgin JSON compression as a
// Traefik middleware plugin. This is a PURE GO implementation (no cgo)
// that produces the exact same wire format as libpidgin C, so any
// JS/Python/Go client can decode the output interchangeably.
//
// Wire format:
//
//	Outer: [brotli_flag:1][payload]
//	  brotli_flag: 0x00 = raw, 0x01 = brotli-compressed payload
//
//	Payload: [type:1][data...]
//	  type 0x01 = single object
//	  type 0x02 = array, followed by [count:u16be]
//
//	Per record per field (in field_order):
//	  If nullable: [marker:1]  0xFF=null, 0x00=absent, 0x01=present
//	  Value encoding by dtype:
//	    bool:      1 byte (0 or 1)
//	    int:       ZigZag varint
//	    float:     8 bytes big-endian IEEE 754 double
//	    str:       [len:u16be][utf8_bytes]
//	    enum:      [idx:1] (0xFE = unknown, then [len:u16be][utf8_bytes])
//	    timestamp: [len:1][utf8_bytes]
//	    object:    recursive record encoding
//	    array:     [count:u16be][records...]
//	  After all known fields:
//	    [unknown_len:u16be][json_fallback_bytes]  (0 if none)
package traefik_plugin_pidgin

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"net/http"
	"sort"
	"strings"
	"sync"

	"github.com/andybalholm/brotli"
)

// ================================================================
// Config + CreateConfig (Traefik plugin interface)
// ================================================================

// Config holds the plugin configuration.
type Config struct {
	LearnSamples   int `json:"learn_samples,omitempty"`
	MinSize        int `json:"min_size,omitempty"`
	EvolveInterval int `json:"evolve_interval,omitempty"`
}

// CreateConfig creates the default plugin configuration.
func CreateConfig() *Config {
	return &Config{
		LearnSamples:   30,
		MinSize:        256,
		EvolveInterval: 500,
	}
}

// ================================================================
// Plugin struct (implements http.Handler)
// ================================================================

// Pidgin is the Traefik middleware plugin.
type Pidgin struct {
	next   http.Handler
	name   string
	config *Config
	engine *Engine
}

// New creates a new Pidgin middleware.
func New(_ context.Context, next http.Handler, config *Config, name string) (http.Handler, error) {
	if config.LearnSamples <= 0 {
		config.LearnSamples = 30
	}
	if config.MinSize <= 0 {
		config.MinSize = 256
	}
	if config.EvolveInterval <= 0 {
		config.EvolveInterval = 500
	}

	return &Pidgin{
		next:   next,
		name:   name,
		config: config,
		engine: NewEngine(config.LearnSamples, config.EvolveInterval),
	}, nil
}

func (p *Pidgin) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	// Serve profile requests
	if strings.HasPrefix(r.URL.Path, "/.well-known/pidgin/") {
		p.serveProfile(w, r)
		return
	}

	// Wrap the response writer to capture the body
	rec := &responseRecorder{
		ResponseWriter: w,
		body:           &bytes.Buffer{},
		statusCode:     http.StatusOK,
		headerWritten:  false,
	}

	p.next.ServeHTTP(rec, r)

	body := rec.body.Bytes()
	ct := rec.Header().Get("Content-Type")
	isJSON := strings.Contains(ct, "json")

	// Pass through non-JSON, non-2xx, or too-small responses
	if !isJSON || rec.statusCode < 200 || rec.statusCode >= 300 || len(body) < p.config.MinSize {
		w.WriteHeader(rec.statusCode)
		w.Write(body)
		return
	}

	// Normalize endpoint
	endpoint := r.URL.Path
	if len(endpoint) > 1 && strings.HasSuffix(endpoint, "/") {
		endpoint = endpoint[:len(endpoint)-1]
	}

	// Always observe for learning
	p.engine.Observe(endpoint, body)

	// Compress if client wants pidgin AND engine is ready
	acceptsPidgin := strings.Contains(r.Header.Get("Accept-Encoding"), "pidgin")

	if acceptsPidgin && p.engine.Ready(endpoint) {
		compressed, err := p.engine.Compress(endpoint, body)
		if err == nil && len(compressed) > 0 && len(compressed) < len(body) {
			ratio := float64(len(compressed)) / float64(len(body)) * 100.0
			w.Header().Set("Content-Encoding", "pidgin")
			w.Header().Set("Content-Length", fmt.Sprintf("%d", len(compressed)))
			w.Header().Set("X-Pidgin-Ratio", fmt.Sprintf("%.1f%%", ratio))
			w.Header().Set("Vary", "Accept-Encoding")
			w.WriteHeader(rec.statusCode)
			w.Write(compressed)
			return
		}
	}

	// Pass through original
	w.WriteHeader(rec.statusCode)
	w.Write(body)
}

func (p *Pidgin) serveProfile(w http.ResponseWriter, r *http.Request) {
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

	profileJSON := p.engine.ProfileJSON(endpoint)
	if profileJSON == nil {
		http.Error(w, `{"error":"no profile for endpoint"}`, http.StatusNotFound)
		return
	}

	etag := p.engine.ETag(endpoint)
	if etag != "" && r.Header.Get("If-None-Match") == etag {
		w.WriteHeader(http.StatusNotModified)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "public, max-age=60")
	if etag != "" {
		w.Header().Set("ETag", etag)
	}
	w.WriteHeader(http.StatusOK)
	w.Write(profileJSON)
}

// ================================================================
// Response recorder
// ================================================================

type responseRecorder struct {
	http.ResponseWriter
	body          *bytes.Buffer
	statusCode    int
	headerWritten bool
}

func (r *responseRecorder) WriteHeader(code int) {
	if !r.headerWritten {
		r.statusCode = code
		r.headerWritten = true
	}
}

func (r *responseRecorder) Write(b []byte) (int, error) {
	return r.body.Write(b)
}

// ================================================================
// Schema types (matches libpidgin C exactly)
// ================================================================

const (
	dtypeBool      = 0
	dtypeInt       = 1
	dtypeFloat     = 2
	dtypeStr       = 3
	dtypeEnum      = 4
	dtypeTimestamp  = 5
	dtypeObject    = 6
	dtypeArray     = 7
	dtypeNull      = 8

	markerNull    = 0xFF
	markerPresent = 0x01
	markerAbsent  = 0x00
)

// Field represents a learned field schema.
type Field struct {
	Name       string   `json:"name,omitempty"`
	Dtype      int      `json:"-"`
	DtypeStr   string   `json:"dtype"`
	Nullable   bool     `json:"nullable"`
	Frequency  float64  `json:"freq"`
	EnumValues []string `json:"enum,omitempty"`

	// Nested object schema
	Nested     []*Field `json:"-"`
	NestedMap  map[string]*Field `json:"nested,omitempty"`
	NestedOrder []string `json:"-"`

	// Array items schema
	Items      []*Field `json:"-"`
	ItemsMap   map[string]*Field `json:"items,omitempty"`
	ItemsOrder []string `json:"-"`
}

// Profile represents a learned schema profile.
type Profile struct {
	Version    int                `json:"version"`
	FieldOrder []string           `json:"field_order"`
	Fields     map[string]*Field  `json:"fields"`
	fieldList  []*Field
}

// ================================================================
// Engine — auto-learning per-endpoint
// ================================================================

type endpointState struct {
	profile  *Profile
	ready    bool
	requests int
	etag     string
	samples  [][]byte
}

// Engine manages per-endpoint auto-learning and compression.
type Engine struct {
	mu              sync.Mutex
	endpoints       map[string]*endpointState
	learnSamples    int
	evolveInterval  int
}

// NewEngine creates a new Pidgin engine.
func NewEngine(learnSamples, evolveInterval int) *Engine {
	return &Engine{
		endpoints:      make(map[string]*endpointState),
		learnSamples:   learnSamples,
		evolveInterval: evolveInterval,
	}
}

func (e *Engine) getOrCreate(endpoint string) *endpointState {
	ep, ok := e.endpoints[endpoint]
	if !ok {
		ep = &endpointState{
			samples: make([][]byte, 0, e.learnSamples),
		}
		e.endpoints[endpoint] = ep
	}
	return ep
}

// Observe feeds a JSON response body to the learning engine.
func (e *Engine) Observe(endpoint string, jsonBody []byte) {
	e.mu.Lock()
	defer e.mu.Unlock()

	ep := e.getOrCreate(endpoint)
	ep.requests++

	if !ep.ready && len(ep.samples) < e.learnSamples {
		// Still learning — store sample
		cp := make([]byte, len(jsonBody))
		copy(cp, jsonBody)
		ep.samples = append(ep.samples, cp)

		if len(ep.samples) >= e.learnSamples {
			// Build profile from collected samples
			profile := learnProfileFromSamples(ep.samples)
			if profile != nil {
				ep.profile = profile
				ep.ready = true
				ep.etag = computeETag(profile)
			}
			ep.samples = nil // free learning buffer
		}
	}
}

// Ready returns whether an endpoint has a built profile.
func (e *Engine) Ready(endpoint string) bool {
	e.mu.Lock()
	defer e.mu.Unlock()
	ep, ok := e.endpoints[endpoint]
	return ok && ep.ready
}

// Compress compresses a JSON body for an endpoint.
func (e *Engine) Compress(endpoint string, jsonBody []byte) ([]byte, error) {
	e.mu.Lock()
	ep, ok := e.endpoints[endpoint]
	if !ok || !ep.ready || ep.profile == nil {
		e.mu.Unlock()
		return nil, fmt.Errorf("not ready")
	}
	profile := ep.profile
	e.mu.Unlock()

	return compressJSON(profile, jsonBody)
}

// ProfileJSON returns the profile as JSON for client download.
func (e *Engine) ProfileJSON(endpoint string) []byte {
	e.mu.Lock()
	defer e.mu.Unlock()

	ep, ok := e.endpoints[endpoint]
	if !ok || !ep.ready || ep.profile == nil {
		return nil
	}

	data, err := json.Marshal(ep.profile)
	if err != nil {
		return nil
	}
	return data
}

// ETag returns the profile ETag for caching.
func (e *Engine) ETag(endpoint string) string {
	e.mu.Lock()
	defer e.mu.Unlock()
	ep, ok := e.endpoints[endpoint]
	if !ok {
		return ""
	}
	return ep.etag
}

// ================================================================
// Profile learning
// ================================================================

func learnProfileFromSamples(samples [][]byte) *Profile {
	// Parse all samples and flatten arrays into individual objects
	var objects []map[string]interface{}
	for _, raw := range samples {
		var parsed interface{}
		if err := json.Unmarshal(raw, &parsed); err != nil {
			continue
		}
		switch v := parsed.(type) {
		case map[string]interface{}:
			// If it has "data" key that's an array, use those items
			if dataArr, ok := v["data"]; ok {
				if arr, ok := dataArr.([]interface{}); ok {
					for _, item := range arr {
						if obj, ok := item.(map[string]interface{}); ok {
							objects = append(objects, obj)
						}
					}
					continue
				}
			}
			objects = append(objects, v)
		case []interface{}:
			for _, item := range v {
				if obj, ok := item.(map[string]interface{}); ok {
					objects = append(objects, obj)
				}
			}
		}
	}

	if len(objects) == 0 {
		return nil
	}

	fields, order := learnFields(objects)
	if len(fields) == 0 {
		return nil
	}

	fieldMap := make(map[string]*Field)
	for _, f := range fields {
		fieldMap[f.Name] = f
	}

	return &Profile{
		Version:    1,
		FieldOrder: order,
		Fields:     fieldMap,
		fieldList:  fields,
	}
}

// learnFields learns the schema from a slice of JSON objects.
// Returns sorted fields and field_order matching libpidgin C behavior.
func learnFields(objects []map[string]interface{}) ([]*Field, []string) {
	type fieldInfo struct {
		name        string
		values      []interface{}
		appearances int
	}

	infoMap := make(map[string]*fieldInfo)
	var infoOrder []string

	for _, obj := range objects {
		for key, val := range obj {
			fi, ok := infoMap[key]
			if !ok {
				fi = &fieldInfo{name: key}
				infoMap[key] = fi
				infoOrder = append(infoOrder, key)
			}
			fi.values = append(fi.values, val)
			fi.appearances++
		}
	}

	nSamples := len(objects)
	var fields []*Field

	for _, key := range infoOrder {
		fi := infoMap[key]
		freq := float64(fi.appearances) / float64(nSamples)
		if freq < 0.1 {
			continue // skip rare fields
		}

		dtype, enumValues := inferDtype(fi.values)

		hasNull := false
		for _, v := range fi.values {
			if v == nil {
				hasNull = true
				break
			}
		}
		nullable := hasNull || freq < 1.0

		f := &Field{
			Name:       key,
			Dtype:      dtype,
			DtypeStr:   dtypeToString(dtype),
			Nullable:   nullable,
			Frequency:  freq,
			EnumValues: enumValues,
		}

		// Handle nested objects
		if dtype == dtypeObject {
			var nestedObjs []map[string]interface{}
			for _, v := range fi.values {
				if obj, ok := v.(map[string]interface{}); ok {
					nestedObjs = append(nestedObjs, obj)
				}
			}
			if len(nestedObjs) > 0 {
				nested, nestedOrder := learnFields(nestedObjs)
				f.Nested = nested
				f.NestedOrder = nestedOrder
				f.NestedMap = make(map[string]*Field)
				for _, nf := range nested {
					f.NestedMap[nf.Name] = nf
				}
			}
		}

		// Handle arrays of objects
		if dtype == dtypeArray {
			var allItems []map[string]interface{}
			nObjItems := 0
			nTotalItems := 0
			for _, v := range fi.values {
				if arr, ok := v.([]interface{}); ok {
					for _, item := range arr {
						nTotalItems++
						if obj, ok := item.(map[string]interface{}); ok {
							nObjItems++
							allItems = append(allItems, obj)
						}
					}
				}
			}
			if nObjItems > 0 && nObjItems > nTotalItems/2 {
				items, itemsOrder := learnFields(allItems)
				f.Items = items
				f.ItemsOrder = itemsOrder
				f.ItemsMap = make(map[string]*Field)
				for _, it := range items {
					f.ItemsMap[it.Name] = it
				}
			}
		}

		fields = append(fields, f)
	}

	// Sort by frequency desc, then alphabetical (matching C qsort behavior)
	sort.SliceStable(fields, func(i, j int) bool {
		if fields[i].Frequency != fields[j].Frequency {
			return fields[i].Frequency > fields[j].Frequency
		}
		return fields[i].Name < fields[j].Name
	})

	order := make([]string, len(fields))
	for i, f := range fields {
		order[i] = f.Name
	}

	return fields, order
}

func inferDtype(values []interface{}) (int, []string) {
	nBool, nInt, nFloat, nStr, nObj, nArr := 0, 0, 0, 0, 0, 0
	nonNull := 0

	for _, v := range values {
		if v == nil {
			continue
		}
		nonNull++
		switch val := v.(type) {
		case bool:
			nBool++
		case float64:
			if val == math.Trunc(val) && val >= -2147483648 && val <= 2147483647 {
				nInt++
			} else {
				nFloat++
			}
		case string:
			nStr++
		case map[string]interface{}:
			nObj++
		case []interface{}:
			nArr++
		}
	}

	if nonNull == 0 {
		return dtypeNull, nil
	}
	if nBool == nonNull {
		return dtypeBool, nil
	}
	if nInt+nFloat == nonNull {
		if nFloat > 0 {
			return dtypeFloat, nil
		}
		return dtypeInt, nil
	}
	if nStr == nonNull {
		// Check timestamp
		for _, v := range values {
			if s, ok := v.(string); ok && len(s) >= 19 {
				if strings.Contains(s, "T") && (strings.HasSuffix(s, "Z") || (len(s) > 10 && strings.Contains(s[10:], "+"))) {
					return dtypeTimestamp, nil
				}
				break
			}
		}

		// Check enum (low cardinality)
		unique := make(map[string]struct{})
		for _, v := range values {
			if s, ok := v.(string); ok {
				unique[s] = struct{}{}
			}
		}
		if len(unique) <= 20 && len(unique) < nonNull/2 {
			enumVals := make([]string, 0, len(unique))
			for s := range unique {
				enumVals = append(enumVals, s)
			}
			sort.Strings(enumVals) // deterministic order
			return dtypeEnum, enumVals
		}

		return dtypeStr, nil
	}
	if nObj == nonNull {
		return dtypeObject, nil
	}
	if nArr == nonNull {
		return dtypeArray, nil
	}

	// Mixed types fallback to string
	return dtypeStr, nil
}

func dtypeToString(d int) string {
	switch d {
	case dtypeBool:
		return "bool"
	case dtypeInt:
		return "int"
	case dtypeFloat:
		return "float"
	case dtypeStr:
		return "str"
	case dtypeEnum:
		return "enum"
	case dtypeTimestamp:
		return "timestamp"
	case dtypeObject:
		return "object"
	case dtypeArray:
		return "array"
	case dtypeNull:
		return "null"
	default:
		return "str"
	}
}

// ================================================================
// Binary encoding (same wire format as libpidgin C)
// ================================================================

type encoder struct {
	buf bytes.Buffer
}

func (e *encoder) u8(v byte) {
	e.buf.WriteByte(v)
}

func (e *encoder) u16be(v uint16) {
	var b [2]byte
	binary.BigEndian.PutUint16(b[:], v)
	e.buf.Write(b[:])
}

func (e *encoder) varint(val int64) {
	// ZigZag encode
	v := uint64((val << 1) ^ (val >> 63))
	for {
		c := byte(v & 0x7F)
		v >>= 7
		if v != 0 {
			c |= 0x80
		}
		e.buf.WriteByte(c)
		if v == 0 {
			break
		}
	}
}

func (e *encoder) float64be(val float64) {
	var b [8]byte
	bits := math.Float64bits(val)
	binary.BigEndian.PutUint64(b[:], bits)
	e.buf.Write(b[:])
}

func (e *encoder) writeBytes(data []byte) {
	e.buf.Write(data)
}

func (e *encoder) encodeValue(val interface{}, schema *Field) error {
	switch schema.Dtype {
	case dtypeBool:
		if b, ok := val.(bool); ok && b {
			e.u8(1)
		} else {
			e.u8(0)
		}

	case dtypeInt:
		var iv int64
		switch v := val.(type) {
		case float64:
			iv = int64(v)
		case int64:
			iv = v
		case int:
			iv = int64(v)
		default:
			iv = 0
		}
		e.varint(iv)

	case dtypeFloat:
		var fv float64
		if v, ok := val.(float64); ok {
			fv = v
		}
		e.float64be(fv)

	case dtypeStr:
		s := ""
		if v, ok := val.(string); ok {
			s = v
		}
		b := []byte(s)
		e.u16be(uint16(len(b)))
		e.writeBytes(b)

	case dtypeEnum:
		s := ""
		if v, ok := val.(string); ok {
			s = v
		}
		found := false
		for idx, ev := range schema.EnumValues {
			if ev == s {
				e.u8(byte(idx))
				found = true
				break
			}
		}
		if !found {
			e.u8(0xFE)
			b := []byte(s)
			e.u16be(uint16(len(b)))
			e.writeBytes(b)
		}

	case dtypeTimestamp:
		s := ""
		if v, ok := val.(string); ok {
			s = v
		}
		b := []byte(s)
		e.u8(byte(len(b)))
		e.writeBytes(b)

	case dtypeObject:
		if schema.Nested != nil {
			if obj, ok := val.(map[string]interface{}); ok {
				return e.encodeObject(obj, schema.Nested, schema.NestedOrder)
			}
		}
		e.u16be(0) // empty unknown fallback

	case dtypeArray:
		if schema.Items != nil {
			if arr, ok := val.([]interface{}); ok {
				e.u16be(uint16(len(arr)))
				for _, item := range arr {
					if obj, ok := item.(map[string]interface{}); ok {
						if err := e.encodeObject(obj, schema.Items, schema.ItemsOrder); err != nil {
							return err
						}
					} else {
						// Primitive array item — JSON fallback
						fb, _ := json.Marshal(item)
						e.u16be(uint16(len(fb)))
						e.writeBytes(fb)
					}
				}
				return nil
			}
		}
		e.u16be(0) // empty array

	default:
		// JSON fallback
		fb, _ := json.Marshal(val)
		e.u16be(uint16(len(fb)))
		e.writeBytes(fb)
	}

	return nil
}

func (e *encoder) encodeObject(obj map[string]interface{}, fields []*Field, order []string) error {
	if obj == nil {
		// All fields absent + zero unknown
		for _, name := range order {
			f := findField(fields, name)
			if f != nil && f.Nullable {
				e.u8(markerAbsent)
			}
		}
		e.u16be(0)
		return nil
	}

	// Build field lookup
	fieldMap := make(map[string]*Field, len(fields))
	for _, f := range fields {
		fieldMap[f.Name] = f
	}

	// Encode known fields in order
	for _, name := range order {
		schema := fieldMap[name]
		if schema == nil {
			continue
		}

		val, exists := obj[name]

		if schema.Nullable {
			if !exists {
				e.u8(markerAbsent)
				continue
			}
			if val == nil {
				e.u8(markerNull)
				continue
			}
			e.u8(markerPresent)
		}

		if err := e.encodeValue(val, schema); err != nil {
			return err
		}
	}

	// Unknown fields: collect fields not in schema
	unknown := make(map[string]interface{})
	knownSet := make(map[string]struct{}, len(fields))
	for _, f := range fields {
		knownSet[f.Name] = struct{}{}
	}
	for key, val := range obj {
		if _, known := knownSet[key]; !known {
			unknown[key] = val
		}
	}

	if len(unknown) > 0 {
		fb, err := json.Marshal(unknown)
		if err != nil {
			e.u16be(0)
		} else {
			e.u16be(uint16(len(fb)))
			e.writeBytes(fb)
		}
	} else {
		e.u16be(0)
	}

	return nil
}

func findField(fields []*Field, name string) *Field {
	for _, f := range fields {
		if f.Name == name {
			return f
		}
	}
	return nil
}

// compressJSON compresses a JSON body using a learned profile.
func compressJSON(profile *Profile, jsonBody []byte) ([]byte, error) {
	var parsed interface{}
	if err := json.Unmarshal(jsonBody, &parsed); err != nil {
		return nil, err
	}

	enc := &encoder{}

	// Build field list and order
	fields := make([]*Field, len(profile.FieldOrder))
	for i, name := range profile.FieldOrder {
		fields[i] = profile.Fields[name]
	}

	switch v := parsed.(type) {
	case map[string]interface{}:
		enc.u8(0x01) // single object
		if err := enc.encodeObject(v, fields, profile.FieldOrder); err != nil {
			return nil, err
		}

	case []interface{}:
		enc.u8(0x02) // array
		enc.u16be(uint16(len(v)))
		for _, item := range v {
			obj, ok := item.(map[string]interface{})
			if !ok {
				obj = nil
			}
			if err := enc.encodeObject(obj, fields, profile.FieldOrder); err != nil {
				return nil, err
			}
		}

	default:
		return nil, fmt.Errorf("unsupported JSON type for pidgin compression")
	}

	payload := enc.buf.Bytes()

	// Try brotli compression
	var brBuf bytes.Buffer
	brWriter := brotli.NewWriterLevel(&brBuf, 6)
	if _, err := brWriter.Write(payload); err == nil {
		if err := brWriter.Close(); err == nil && brBuf.Len() < len(payload) {
			// Brotli is smaller
			result := make([]byte, 1+brBuf.Len())
			result[0] = 0x01 // brotli flag
			copy(result[1:], brBuf.Bytes())
			return result, nil
		}
	}

	// Raw (no brotli or brotli didn't help)
	result := make([]byte, 1+len(payload))
	result[0] = 0x00 // no brotli
	copy(result[1:], payload)
	return result, nil
}

// ================================================================
// Helpers
// ================================================================

func computeETag(profile *Profile) string {
	data, err := json.Marshal(profile)
	if err != nil {
		return ""
	}
	// DJB2 hash (same as C implementation)
	var hash uint64 = 5381
	for _, b := range data {
		hash = ((hash << 5) + hash) + uint64(b)
	}
	return fmt.Sprintf("\"%x\"", hash)
}

// Ensure io is used (brotli reader needs it for potential decompress)
var _ = io.EOF
