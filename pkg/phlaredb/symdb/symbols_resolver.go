package symdb

import (
	"context"
	"sync"

	"github.com/google/pprof/profile"

	v1 "github.com/grafana/pyroscope/pkg/phlaredb/schemas/v1"
)

type StacktraceResolver interface {
	// ResolveStacktraces resolves locations for each stack trace
	// and inserts it to the StacktraceInserter provided.
	//
	// The stacktraces must be ordered in the ascending order.
	// If a stacktrace can't be resolved, dst receives an empty
	// array of locations.
	//
	// Stacktraces slice might be modified during the call.
	ResolveStacktraces(ctx context.Context, dst StacktraceInserter, stacktraces []uint32) error
}

// StacktraceInserter accepts resolved locations for a given stack trace.
// The leaf is at locations[0].
//
// Locations slice must not be retained by implementation.
// It is guaranteed, that for a given stacktrace ID
// InsertStacktrace is called not more than once.
type StacktraceInserter interface {
	InsertStacktrace(stacktraceID uint32, locations []int32)
}

type StacktraceInserterFn func(stacktraceID uint32, locations []int32)

func (fn StacktraceInserterFn) InsertStacktrace(stacktraceID uint32, locations []int32) {
	fn(stacktraceID, locations)
}

type Resolver struct {
	Stacktraces StacktraceResolver
	Locations   []*v1.InMemoryLocation
	Mappings    []*v1.InMemoryMapping
	Functions   []*v1.InMemoryFunction
	Strings     []string
}

func (r *Resolver) ResolveProfile(ctx context.Context, samples v1.Samples) (*profile.Profile, error) {
	t := pprofResolveFromPool()
	defer t.reset()
	t.init(r, samples)
	if err := r.Stacktraces.ResolveStacktraces(ctx, t, samples.StacktraceIDs); err != nil {
		return nil, err
	}
	return t.profile, nil
}

type pprofResolve struct {
	profile  *profile.Profile
	resolver *Resolver
	samples  *v1.Samples
	cur      int

	locations []*profile.Location
	mappings  []*profile.Mapping
	functions []*profile.Function
}

var pprofResolvePool = sync.Pool{
	New: func() any { return new(pprofResolve) },
}

func pprofResolveFromPool() *pprofResolve {
	return pprofResolvePool.Get().(*pprofResolve)
}

func (r *pprofResolve) reset() {
	r.profile = nil
	r.resolver = nil
	r.samples = nil
	r.cur = 0
	clear(r.locations)
	clear(r.mappings)
	clear(r.functions)
	pprofResolvePool.Put(r)
}

func (r *pprofResolve) init(resolver *Resolver, samples v1.Samples) {
	r.resolver = resolver
	r.samples = &samples
	r.profile = &profile.Profile{
		Sample: make([]*profile.Sample, len(samples.StacktraceIDs)),
	}
	r.locations = grow(r.locations, len(r.resolver.Locations))
	r.mappings = grow(r.mappings, len(r.resolver.Mappings))
	r.functions = grow(r.functions, len(r.resolver.Functions))
}

func grow[T any](s []T, n int) []T {
	if cap(s) < n {
		s = make([]T, n)
	}
	s = s[:n]
	return s
}

func clear[T any](s []T) {
	var zero T
	for i := range s {
		s[i] = zero
	}
}

func (r *pprofResolve) InsertStacktrace(_ uint32, locations []int32) {
	var sample profile.Sample
	sample.Value = []int64{int64(r.samples.Values[r.cur])}
	sample.Location = make([]*profile.Location, len(locations))
	for j, loc := range locations {
		sample.Location[j] = r.location(loc)
	}
	r.profile.Sample[r.cur] = &sample
	r.cur++
}

func (r *pprofResolve) location(i int32) *profile.Location {
	if x := r.locations[i]; x != nil {
		return x
	}
	loc := r.inMemoryLocationToPprof(r.resolver.Locations[i])
	r.profile.Location = append(r.profile.Location, loc)
	r.locations[i] = loc
	return loc
}

func (r *pprofResolve) mapping(i uint32) *profile.Mapping {
	if x := r.mappings[i]; x != nil {
		return x
	}
	m := r.inMemoryMappingToPprof(r.resolver.Mappings[i])
	r.profile.Mapping = append(r.profile.Mapping, m)
	r.mappings[i] = m
	return m
}

func (r *pprofResolve) function(i uint32) *profile.Function {
	if x := r.functions[i]; x != nil {
		return x
	}
	f := r.inMemoryFunctionToPprof(r.resolver.Functions[i])
	r.profile.Function = append(r.profile.Function, f)
	r.functions[i] = f
	return f
}

func (r *pprofResolve) inMemoryLocationToPprof(m *v1.InMemoryLocation) *profile.Location {
	x := &profile.Location{
		ID:       m.Id,
		Mapping:  r.mapping(m.MappingId),
		Address:  m.Address,
		IsFolded: m.IsFolded,
	}
	x.Line = make([]profile.Line, len(m.Line))
	for i, line := range m.Line {
		x.Line[i] = profile.Line{
			Function: r.function(line.FunctionId),
			Line:     int64(line.Line),
		}
	}
	return x
}

func (r *pprofResolve) inMemoryMappingToPprof(m *v1.InMemoryMapping) *profile.Mapping {
	return &profile.Mapping{
		ID:              m.Id,
		Start:           m.MemoryStart,
		Limit:           m.MemoryLimit,
		Offset:          m.FileOffset,
		File:            r.resolver.Strings[m.Filename],
		BuildID:         r.resolver.Strings[m.BuildId],
		HasFunctions:    m.HasFunctions,
		HasFilenames:    m.HasFilenames,
		HasLineNumbers:  m.HasLineNumbers,
		HasInlineFrames: m.HasInlineFrames,
	}
}

func (r *pprofResolve) inMemoryFunctionToPprof(m *v1.InMemoryFunction) *profile.Function {
	return &profile.Function{
		ID:         m.Id,
		Name:       r.resolver.Strings[m.Name],
		SystemName: r.resolver.Strings[m.SystemName],
		Filename:   r.resolver.Strings[m.Filename],
		StartLine:  int64(m.StartLine),
	}
}
