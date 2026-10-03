"""Independent M0 diagnostic scorer; M4 offline evaluation lives in evaluation/."""
import unicodedata

POLICY = "m0_nfc_casefold_punctuation_v1"


def normalize(text, language):
    text = unicodedata.normalize("NFC", text)
    if language != "zh":
        text = text.casefold()
    # Preserve all originals; punctuation removal is an explicit scoring policy.
    text = "".join(("" if language == "zh" else " ") if unicodedata.category(char).startswith("P") else char for char in text)
    return "".join(text.split()) if language == "zh" else " ".join(text.split())


def score(reference, hypothesis, language):
    import jiwer
    ref, hyp = normalize(reference, language), normalize(hypothesis, language)
    result = jiwer.process_characters(ref, hyp) if language == "zh" else jiwer.process_words(ref, hyp)
    denominator = len(ref) if language == "zh" else len(ref.split())
    return {"metric": "cer" if language == "zh" else "wer", "policy": POLICY,
            "normalized_reference": ref, "normalized_hypothesis": hyp,
            "substitutions": result.substitutions, "insertions": result.insertions,
            "deletions": result.deletions, "reference_units": denominator,
            "rate": (result.substitutions + result.insertions + result.deletions) / denominator if denominator else None}
