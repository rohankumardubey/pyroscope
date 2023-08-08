package symdb

import (
	"context"
	"testing"

	"github.com/stretchr/testify/mock"
	"github.com/stretchr/testify/require"

	"github.com/grafana/pyroscope/pkg/objstore/providers/filesystem"
	"github.com/grafana/pyroscope/pkg/phlaredb/block"
	schemav1 "github.com/grafana/pyroscope/pkg/phlaredb/schemas/v1"
)

var testBlockMeta = block.Meta{
	Files: []block.File{
		{RelPath: IndexFileName},
		{RelPath: StacktracesFileName},
		{RelPath: "locations.parquet"},
		{RelPath: "mappings.parquet"},
		{RelPath: "functions.parquet"},
		{RelPath: "strings.parquet"},
	},
}

func Test_Reader_Open(t *testing.T) {
	// TODO: Read db from disk.
	cfg := &Config{
		Dir: t.TempDir(),
		Stacktraces: StacktracesConfig{
			MaxNodesPerChunk: 7,
		},
		Parquet: ParquetConfig{
			MaxBufferRowCount: 100 << 10,
		},
	}

	db := NewSymDB(cfg)
	w := db.SymbolsWriter(1)
	sids := make([]uint32, 5)
	w.AppendStacktraces(sids, []*schemav1.Stacktrace{
		{LocationIDs: []uint64{3, 2, 1}},
		{LocationIDs: []uint64{2, 1}},
		{LocationIDs: []uint64{4, 3, 2, 1}},
		{LocationIDs: []uint64{3, 1}},
		{LocationIDs: []uint64{5, 2, 1}},
	})
	require.Equal(t, []uint32{3, 2, 11, 16, 18}, sids)
	require.NoError(t, db.Flush())
	t.Log(db.Files())

	b, err := filesystem.NewBucket(cfg.Dir)
	require.NoError(t, err)
	x, err := Open(context.Background(), b, testBlockMeta)
	require.NoError(t, err)
	r, err := x.SymbolsReader(context.Background(), 1)
	require.NoError(t, err)

	dst := new(mockStacktraceInserter)
	dst.On("InsertStacktrace", uint32(2), []int32{2, 1})
	dst.On("InsertStacktrace", uint32(3), []int32{3, 2, 1})
	dst.On("InsertStacktrace", uint32(11), []int32{4, 3, 2, 1})
	dst.On("InsertStacktrace", uint32(16), []int32{3, 1})
	dst.On("InsertStacktrace", uint32(18), []int32{5, 2, 1})

	err = r.ResolveStacktraceLocations(context.Background(), dst, sids)
	require.NoError(t, err)
}

func Test_Reader_Open_v1(t *testing.T) {
	b, err := filesystem.NewBucket("testdata/symbols/v1")
	require.NoError(t, err)
	x, err := Open(context.Background(), b, testBlockMeta)
	require.NoError(t, err)
	r, err := x.SymbolsReader(context.Background(), 1)
	require.NoError(t, err)

	dst := new(mockStacktraceInserter)
	dst.On("InsertStacktrace", uint32(2), []int32{2, 1})
	dst.On("InsertStacktrace", uint32(3), []int32{3, 2, 1})
	dst.On("InsertStacktrace", uint32(11), []int32{4, 3, 2, 1})
	dst.On("InsertStacktrace", uint32(16), []int32{3, 1})
	dst.On("InsertStacktrace", uint32(18), []int32{5, 2, 1})

	err = r.ResolveStacktraceLocations(context.Background(), dst, []uint32{3, 2, 11, 16, 18})
	require.NoError(t, err)
}

type mockStacktraceInserter struct{ mock.Mock }

func (m *mockStacktraceInserter) InsertStacktrace(stacktraceID uint32, locations []int32) {
	m.Called(stacktraceID, locations)
}
