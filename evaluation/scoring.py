"""Offline accuracy contracts. Inference and timing remain in the C++ runner."""
from array import array
from typing import Protocol
import unicodedata

POLICY = "m0_nfc_casefold_punctuation_v1"


class ITextNormalizer(Protocol):
    policy: str

    def normalize(self, text: str, language: str) -> str: ...


class MultilingualNormalizer:
    policy = POLICY

    def normalize(self, text, language):
        if language not in ("en", "id", "zh"):
            raise ValueError(f"unsupported scoring language: {language}")
        text.encode("utf-8", errors="strict")
        text = unicodedata.normalize("NFC", text)
        if language != "zh":
            text = text.casefold()
        text = "".join(("" if language == "zh" else " ")
                       if unicodedata.category(char).startswith("P") else char for char in text)
        return "".join(text.split()) if language == "zh" else " ".join(text.split())


class IdentityNormalizer:
    policy = "raw_identity_v1"

    def normalize(self, text, language):
        if language not in ("en", "id", "zh"):
            raise ValueError(f"unsupported scoring language: {language}")
        text.encode("utf-8", errors="strict")
        return text


class AccuracyEvaluator:
    """Unit-cost Levenshtein with auditable alignment; never combines WER and CER."""

    def __init__(self, normalizer=None):
        self.normalizer = normalizer or MultilingualNormalizer()

    def score(self, reference, hypothesis, language):
        ref = self.normalizer.normalize(reference, language)
        hyp = self.normalizer.normalize(hypothesis, language)
        r = list(ref) if language == "zh" else ref.split()
        h = list(hyp) if language == "zh" else hyp.split()
        if max(len(r), len(h)) > 4000 or len(r) * len(h) > 4_000_000:
            raise ValueError("alignment exceeds bounded evaluator capacity")
        matrix = [array("I", range(len(h) + 1))]
        for i, left in enumerate(r, 1):
            row = array("I", [i])
            previous = matrix[-1]
            for j, right in enumerate(h, 1):
                row.append(min(previous[j] + 1, row[j - 1] + 1,
                               previous[j - 1] + (left != right)))
            matrix.append(row)
        operations = []
        i, j = len(r), len(h)
        counts = {"substitutions": 0, "deletions": 0, "insertions": 0}
        # Tie rule: diagonal (equal/substitute), delete, insert. Independent
        # evaluators may choose another equally minimal S/D/I alignment.
        while i or j:
            if i and j and matrix[i][j] == matrix[i - 1][j - 1] + (r[i - 1] != h[j - 1]):
                i -= 1
                j -= 1
                op = "equal" if r[i] == h[j] else "substitute"
                counts["substitutions"] += op == "substitute"
                operations.append({"op": op, "reference_index": i, "hypothesis_index": j,
                                   "reference": r[i], "hypothesis": h[j]})
            elif i and matrix[i][j] == matrix[i - 1][j] + 1:
                i -= 1
                counts["deletions"] += 1
                operations.append({"op": "delete", "reference_index": i, "hypothesis_index": j,
                                   "reference": r[i], "hypothesis": None})
            else:
                j -= 1
                counts["insertions"] += 1
                operations.append({"op": "insert", "reference_index": i, "hypothesis_index": j,
                                   "reference": None, "hypothesis": h[j]})
        edits = sum(counts.values())
        return {"metric": "cer" if language == "zh" else "wer", "policy": self.normalizer.policy,
                "unicode_version": unicodedata.unidata_version,
                "reference": reference, "hypothesis": hypothesis,
                "normalized_reference": ref, "normalized_hypothesis": hyp,
                **counts, "edits": edits, "reference_units": len(r),
                "rate": edits / len(r) if r else None,
                "alignment": list(reversed(operations)), "alignment_tie_rule": "diagonal_delete_insert"}


def aggregate(rows):
    """Rows contain language, status, and accuracy; failure hypotheses must be empty."""
    result = {}
    for language in ("en", "id", "zh"):
        offered = [row for row in rows if row["language"] == language]
        result[language] = {"metric": "cer" if language == "zh" else "wer"}
        for population, selected in (("offered_failure_inclusive", offered),
                                     ("completed_only", [r for r in offered if r["status"] == "COMPLETE"])):
            edits = sum(row["accuracy"]["edits"] for row in selected)
            units = sum(row["accuracy"]["reference_units"] for row in selected)
            result[language][population] = {"calls": len(selected), "edits": edits, "reference_units": units,
                                            "rate": edits / units if units else None}
        result[language]["failures"] = sum(row["status"] != "COMPLETE" for row in offered)
    return result
