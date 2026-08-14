package main

import (
	"bytes"
	"encoding/json"
	"path/filepath"
	"strings"
	"testing"

	bolt "go.etcd.io/bbolt"
)

func TestExportRecordsToWriterExportsRequestedEcosystem(t *testing.T) {
	db := openTestDB(t)
	defer db.Close()

	published := "2021-12-10T00:00:00Z"
	modified := "2021-12-11T00:00:00Z"
	seedAdvisoryData(t, db,
		"CVE-2021-44228",
		"maven::github.com/apache/logging-log4j2",
		"org.apache.logging.log4j:log4j-core",
		advisoryEntry{
			VendorIDs:          []string{"GHSA-jfh8-c2jp-5v3q", "GHSA-jfh8-c2jp-5v3q"},
			VulnerableVersions: []string{"=2.14.1", ">=2.0-beta9,<2.15.0"},
			PatchedVersions:    []string{"2.15.0"},
			Severity:           1,
		},
		map[string]vulnerabilityDetail{
			"nvd": {
				CvssScoreV3:      10.0,
				SeverityV3:       4,
				References:       []string{"https://nvd.example/CVE-2021-44228", "https://nvd.example/CVE-2021-44228"},
				Title:            "Remote code execution in Log4j",
				Description:      "Log4Shell",
				PublishedDate:    &published,
				LastModifiedDate: &modified,
			},
		},
	)
	seedAdvisoryData(t, db,
		"CVE-2022-0001",
		"cargo::crates.io/rand",
		"rand",
		advisoryEntry{VulnerableVersions: []string{"=0.8.5"}},
		nil,
	)

	var output bytes.Buffer
	err := db.View(func(tx *bolt.Tx) error {
		return exportRecordsToWriter(tx, map[string]struct{}{"Maven": {}}, &output)
	})
	if err != nil {
		t.Fatalf("exportRecordsToWriter returned error: %v", err)
	}

	lines := nonEmptyLines(output.String())
	if len(lines) != 1 {
		t.Fatalf("expected 1 output line, got %d: %q", len(lines), output.String())
	}

	var record helperRecord
	if err := json.Unmarshal([]byte(lines[0]), &record); err != nil {
		t.Fatalf("failed to decode helper record: %v", err)
	}

	if record.Ecosystem != "Maven" {
		t.Fatalf("unexpected ecosystem: %q", record.Ecosystem)
	}
	if record.PackageName != "org.apache.logging.log4j:log4j-core" {
		t.Fatalf("unexpected package name: %q", record.PackageName)
	}
	if record.AdvisoryID != "CVE-2021-44228" {
		t.Fatalf("unexpected advisory id: %q", record.AdvisoryID)
	}
	if record.Severity != "critical" {
		t.Fatalf("unexpected severity: %q", record.Severity)
	}
	if record.Score != 10.0 {
		t.Fatalf("unexpected score: %v", record.Score)
	}
	if record.Summary != "Remote code execution in Log4j" {
		t.Fatalf("unexpected summary: %q", record.Summary)
	}
	if record.Description != "Log4Shell" {
		t.Fatalf("unexpected description: %q", record.Description)
	}
	if record.Published != published {
		t.Fatalf("unexpected published date: %q", record.Published)
	}
	if record.Modified != modified {
		t.Fatalf("unexpected modified date: %q", record.Modified)
	}
	if joined := strings.Join(record.Aliases, ","); joined != "GHSA-jfh8-c2jp-5v3q" {
		t.Fatalf("unexpected aliases: %q", joined)
	}
	if joined := strings.Join(record.References, ","); joined != "https://nvd.example/CVE-2021-44228" {
		t.Fatalf("unexpected references: %q", joined)
	}
	if joined := strings.Join(record.Versions, ","); joined != "2.14.1" {
		t.Fatalf("unexpected versions: %q", joined)
	}
	if len(record.Ranges) != 1 {
		t.Fatalf("unexpected range count: %d", len(record.Ranges))
	}
	if record.Ranges[0].Introduced != "2.0-beta9" || record.Ranges[0].Fixed != "2.15.0" {
		t.Fatalf("unexpected range: %+v", record.Ranges[0])
	}
}

func TestExportRecordsToWriterFallsBackToPatchedVersionRange(t *testing.T) {
	db := openTestDB(t)
	defer db.Close()

	seedAdvisoryData(t, db,
		"CVE-2023-1234",
		"debian 11",
		"openssl",
		advisoryEntry{
			PatchedVersions: []string{"1.1.1n"},
		},
		nil,
	)

	var output bytes.Buffer
	err := db.View(func(tx *bolt.Tx) error {
		return exportRecordsToWriter(tx, map[string]struct{}{"Debian": {}}, &output)
	})
	if err != nil {
		t.Fatalf("exportRecordsToWriter returned error: %v", err)
	}

	lines := nonEmptyLines(output.String())
	if len(lines) != 1 {
		t.Fatalf("expected 1 output line, got %d: %q", len(lines), output.String())
	}

	var record helperRecord
	if err := json.Unmarshal([]byte(lines[0]), &record); err != nil {
		t.Fatalf("failed to decode helper record: %v", err)
	}

	if record.Ecosystem != "Debian" {
		t.Fatalf("unexpected ecosystem: %q", record.Ecosystem)
	}
	if len(record.Ranges) != 1 {
		t.Fatalf("unexpected range count: %d", len(record.Ranges))
	}
	if record.Ranges[0].Introduced != "0" || record.Ranges[0].Fixed != "1.1.1n" {
		t.Fatalf("unexpected range fallback: %+v", record.Ranges[0])
	}
	if len(record.Versions) != 0 {
		t.Fatalf("expected no explicit versions, got: %+v", record.Versions)
	}
}

func openTestDB(t *testing.T) *bolt.DB {
	t.Helper()
	path := filepath.Join(t.TempDir(), "trivy.db")
	db, err := bolt.Open(path, 0o600, nil)
	if err != nil {
		t.Fatalf("failed to open test db: %v", err)
	}
	return db
}

func seedAdvisoryData(
	t *testing.T,
	db *bolt.DB,
	vulnID string,
	bucketName string,
	packageName string,
	advisory advisoryEntry,
	details map[string]vulnerabilityDetail,
) {
	t.Helper()
	err := db.Update(func(tx *bolt.Tx) error {
		advisoryRoot, err := tx.CreateBucketIfNotExists([]byte("advisory-detail"))
		if err != nil {
			return err
		}
		vulnRoot, err := tx.CreateBucketIfNotExists([]byte("vulnerability-detail"))
		if err != nil {
			return err
		}

		vulnBucket, err := advisoryRoot.CreateBucketIfNotExists([]byte(vulnID))
		if err != nil {
			return err
		}
		pkgBucket, err := vulnBucket.CreateBucketIfNotExists([]byte(bucketName))
		if err != nil {
			return err
		}
		encodedAdvisory, err := json.Marshal(advisory)
		if err != nil {
			return err
		}
		if err := pkgBucket.Put([]byte(packageName), encodedAdvisory); err != nil {
			return err
		}

		if len(details) == 0 {
			return nil
		}
		detailBucket, err := vulnRoot.CreateBucketIfNotExists([]byte(vulnID))
		if err != nil {
			return err
		}
		for sourceID, detail := range details {
			encodedDetail, err := json.Marshal(detail)
			if err != nil {
				return err
			}
			if err := detailBucket.Put([]byte(sourceID), encodedDetail); err != nil {
				return err
			}
		}
		return nil
	})
	if err != nil {
		t.Fatalf("failed to seed advisory data: %v", err)
	}
}

func nonEmptyLines(value string) []string {
	parts := strings.Split(value, "\n")
	lines := make([]string, 0, len(parts))
	for _, part := range parts {
		part = strings.TrimSpace(part)
		if part != "" {
			lines = append(lines, part)
		}
	}
	return lines
}
