package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"sort"
	"strings"

	bolt "go.etcd.io/bbolt"
)

type helperRange struct {
	Introduced   string `json:"introduced,omitempty"`
	Fixed        string `json:"fixed,omitempty"`
	LastAffected string `json:"lastAffected,omitempty"`
	Limit        string `json:"limit,omitempty"`
}

type helperRecord struct {
	Ecosystem   string        `json:"ecosystem"`
	PackageName string        `json:"packageName"`
	AdvisoryID  string        `json:"advisoryId"`
	Aliases     []string      `json:"aliases,omitempty"`
	Summary     string        `json:"summary,omitempty"`
	Description string        `json:"description,omitempty"`
	Severity    string        `json:"severity,omitempty"`
	Score       float64       `json:"score,omitempty"`
	References  []string      `json:"references,omitempty"`
	Modified    string        `json:"modified,omitempty"`
	Published   string        `json:"published,omitempty"`
	Versions    []string      `json:"versions,omitempty"`
	Ranges      []helperRange `json:"ranges,omitempty"`
}

type advisoryEntry struct {
	VendorIDs          []string `json:",omitempty"`
	VulnerableVersions []string `json:",omitempty"`
	PatchedVersions    []string `json:",omitempty"`
	AffectedVersion    string   `json:",omitempty"`
	FixedVersion       string   `json:",omitempty"`
	Severity           int      `json:",omitempty"`
}

type vulnerabilityDetail struct {
	CvssScore        float64  `json:",omitempty"`
	CvssVector       string   `json:",omitempty"`
	CvssScoreV3      float64  `json:",omitempty"`
	CvssVectorV3     string   `json:",omitempty"`
	CvssScoreV40     float64  `json:",omitempty"`
	CvssVectorV40    string   `json:",omitempty"`
	Severity         int      `json:",omitempty"`
	SeverityV3       int      `json:",omitempty"`
	SeverityV40      int      `json:",omitempty"`
	References       []string `json:",omitempty"`
	Title            string   `json:",omitempty"`
	Description      string   `json:",omitempty"`
	PublishedDate    *string  `json:",omitempty"`
	LastModifiedDate *string  `json:",omitempty"`
}

var severityNames = []string{"unknown", "low", "medium", "high", "critical"}

type multiFlag []string

func (m *multiFlag) String() string {
	return strings.Join(*m, ",")
}

func (m *multiFlag) Set(value string) error {
	*m = append(*m, value)
	return nil
}

func main() {
	var dbPath string
	var ecosystems multiFlag
	flag.StringVar(&dbPath, "db-path", "", "path to trivy.db")
	flag.Var(&ecosystems, "ecosystem", "requested ecosystem")
	flag.Parse()

	if dbPath == "" {
		fail(errors.New("--db-path required"))
	}
	if len(ecosystems) == 0 {
		fail(errors.New("at least one --ecosystem required"))
	}

	requested := map[string]struct{}{}
	for _, eco := range ecosystems {
		requested[strings.TrimSpace(eco)] = struct{}{}
	}

	db, err := bolt.Open(dbPath, 0o644, &bolt.Options{ReadOnly: true})
	if err != nil {
		fail(err)
	}
	defer db.Close()

	if err := db.View(func(tx *bolt.Tx) error {
		return exportRecords(tx, requested)
	}); err != nil {
		fail(err)
	}
}

func fail(err error) {
	_, _ = fmt.Fprintln(os.Stderr, err.Error())
	os.Exit(1)
}

func exportRecords(tx *bolt.Tx, requested map[string]struct{}) error {
	return exportRecordsToWriter(tx, requested, os.Stdout)
}

func exportRecordsToWriter(tx *bolt.Tx, requested map[string]struct{}, writer io.Writer) error {
	advisoryRoot := tx.Bucket([]byte("advisory-detail"))
	if advisoryRoot == nil {
		return nil
	}
	vulnRoot := tx.Bucket([]byte("vulnerability-detail"))

	return advisoryRoot.ForEach(func(vulnIDBytes, _ []byte) error {
		vulnID := string(vulnIDBytes)
		vulnBucket := advisoryRoot.Bucket(vulnIDBytes)
		if vulnBucket == nil {
			return nil
		}

		return vulnBucket.ForEach(func(bucketNameBytes, _ []byte) error {
			bucketName := string(bucketNameBytes)
			ecosystem, ok := mapBucketEcosystem(bucketName)
			if !ok {
				return nil
			}
			if _, wanted := requested[ecosystem]; !wanted {
				return nil
			}

			packageBucket := vulnBucket.Bucket(bucketNameBytes)
			if packageBucket == nil {
				return nil
			}
			details := readVulnerabilityDetail(vulnRoot, vulnID)
			return packageBucket.ForEach(func(packageNameBytes, value []byte) error {
				if value == nil {
					return nil
				}
				var advisory advisoryEntry
				if err := json.Unmarshal(value, &advisory); err != nil {
					return err
				}
				record := buildRecord(ecosystem, string(packageNameBytes), vulnID, advisory, details)
				encoded, err := json.Marshal(record)
				if err != nil {
					return err
				}
				_, err = fmt.Fprintln(writer, string(encoded))
				return err
			})
		})
	})
}

func mapBucketEcosystem(bucket string) (string, bool) {
	bucket = strings.ToLower(strings.TrimSpace(bucket))
	switch {
	case strings.HasPrefix(bucket, "maven::"):
		return "Maven", true
	case strings.HasPrefix(bucket, "cargo::"):
		return "crates.io", true
	case strings.HasPrefix(bucket, "debian"):
		return "Debian", true
	case strings.HasPrefix(bucket, "red hat"), strings.HasPrefix(bucket, "rpm::"):
		return "RPM", true
	default:
		return "", false
	}
}

func readVulnerabilityDetail(root *bolt.Bucket, vulnID string) map[string]vulnerabilityDetail {
	result := map[string]vulnerabilityDetail{}
	if root == nil {
		return result
	}
	vulnBucket := root.Bucket([]byte(vulnID))
	if vulnBucket == nil {
		return result
	}
	_ = vulnBucket.ForEach(func(sourceID, value []byte) error {
		if value == nil {
			return nil
		}
		var detail vulnerabilityDetail
		if err := json.Unmarshal(value, &detail); err == nil {
			result[string(sourceID)] = detail
		}
		return nil
	})
	return result
}

func buildRecord(ecosystem, packageName, vulnID string, adv advisoryEntry, details map[string]vulnerabilityDetail) helperRecord {
	references := []string{}
	title := ""
	description := ""
	modified := ""
	published := ""
	severity := severityString(adv.Severity)
	score := 0.0

	for _, source := range sortedKeys(details) {
		detail := details[source]
		references = append(references, detail.References...)
		if title == "" && detail.Title != "" {
			title = detail.Title
		}
		if description == "" && detail.Description != "" {
			description = detail.Description
		}
		if published == "" && detail.PublishedDate != nil {
			published = *detail.PublishedDate
		}
		if modified == "" && detail.LastModifiedDate != nil {
			modified = *detail.LastModifiedDate
		}
		sev, sevScore := bestSeverity(detail)
		if severityRank(sev) > severityRank(severity) {
			severity = sev
		}
		if sevScore > score {
			score = sevScore
		}
	}

	versions, ranges := normalizeVersions(adv)
	return helperRecord{
		Ecosystem:   ecosystem,
		PackageName: packageName,
		AdvisoryID:  vulnID,
		Aliases:     uniqueStrings(adv.VendorIDs),
		Summary:     title,
		Description: description,
		Severity:    severity,
		Score:       score,
		References:  uniqueStrings(references),
		Modified:    modified,
		Published:   published,
		Versions:    versions,
		Ranges:      ranges,
	}
}

func normalizeVersions(adv advisoryEntry) ([]string, []helperRange) {
	versionsSet := map[string]struct{}{}
	ranges := []helperRange{}
	for _, expr := range adv.VulnerableVersions {
		expr = strings.TrimSpace(expr)
		if expr == "" {
			continue
		}
		switch {
		case strings.HasPrefix(expr, "="):
			versionsSet[strings.TrimSpace(strings.TrimPrefix(expr, "="))] = struct{}{}
		case strings.HasPrefix(expr, ">=") && !strings.Contains(expr, ","):
			ranges = append(ranges, helperRange{Introduced: strings.TrimSpace(strings.TrimPrefix(expr, ">="))})
		case strings.Contains(expr, ","):
			parts := strings.Split(expr, ",")
			var r helperRange
			for _, part := range parts {
				part = strings.TrimSpace(part)
				switch {
				case strings.HasPrefix(part, ">="):
					r.Introduced = strings.TrimSpace(strings.TrimPrefix(part, ">="))
				case strings.HasPrefix(part, "<="):
					r.LastAffected = strings.TrimSpace(strings.TrimPrefix(part, "<="))
				case strings.HasPrefix(part, "<"):
					r.Fixed = strings.TrimSpace(strings.TrimPrefix(part, "<"))
				}
			}
			if r.Introduced != "" || r.Fixed != "" || r.LastAffected != "" || r.Limit != "" {
				ranges = append(ranges, r)
			}
		default:
			versionsSet[expr] = struct{}{}
		}
	}
	if adv.AffectedVersion != "" {
		versionsSet[adv.AffectedVersion] = struct{}{}
	}
	if adv.FixedVersion != "" && len(ranges) == 0 {
		ranges = append(ranges, helperRange{Introduced: "0", Fixed: adv.FixedVersion})
	}
	if len(ranges) == 0 {
		for _, patched := range adv.PatchedVersions {
			if patched != "" {
				ranges = append(ranges, helperRange{Introduced: "0", Fixed: patched})
			}
		}
	}
	versions := make([]string, 0, len(versionsSet))
	for version := range versionsSet {
		versions = append(versions, version)
	}
	sort.Strings(versions)
	return versions, ranges
}

func sortedKeys[V any](input map[string]V) []string {
	keys := make([]string, 0, len(input))
	for key := range input {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	return keys
}

func uniqueStrings(values []string) []string {
	seen := map[string]struct{}{}
	result := make([]string, 0, len(values))
	for _, value := range values {
		value = strings.TrimSpace(value)
		if value == "" {
			continue
		}
		if _, ok := seen[value]; ok {
			continue
		}
		seen[value] = struct{}{}
		result = append(result, value)
	}
	sort.Strings(result)
	return result
}

func bestSeverity(detail vulnerabilityDetail) (string, float64) {
	if detail.SeverityV40 > 0 {
		return severityString(detail.SeverityV40), detail.CvssScoreV40
	}
	if detail.SeverityV3 > 0 {
		return severityString(detail.SeverityV3), detail.CvssScoreV3
	}
	if detail.Severity > 0 {
		return severityString(detail.Severity), detail.CvssScore
	}
	if detail.CvssScoreV40 > 0 {
		return scoreSeverity(detail.CvssScoreV40), detail.CvssScoreV40
	}
	if detail.CvssScoreV3 > 0 {
		return scoreSeverity(detail.CvssScoreV3), detail.CvssScoreV3
	}
	if detail.CvssScore > 0 {
		return scoreSeverity(detail.CvssScore), detail.CvssScore
	}
	return "unknown", 0
}

func severityString(value int) string {
	if value >= 0 && value < len(severityNames) {
		return severityNames[value]
	}
	return "unknown"
}

func scoreSeverity(score float64) string {
	switch {
	case score >= 9.0:
		return "critical"
	case score >= 7.0:
		return "high"
	case score >= 4.0:
		return "medium"
	case score > 0:
		return "low"
	default:
		return "unknown"
	}
}

func severityRank(value string) int {
	switch strings.ToLower(value) {
	case "critical":
		return 4
	case "high":
		return 3
	case "medium":
		return 2
	case "low":
		return 1
	default:
		return 0
	}
}
