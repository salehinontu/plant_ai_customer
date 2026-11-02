"""Plant health diagnosis with Claude vision.

Without ANTHROPIC_API_KEY the module returns a clearly-labelled demo result so the website can be
tried out; with a key it sends the photo (plus the soil-sensor reading) to Claude and gets back JSON
that follows SCHEMA.
"""
import base64
import itertools
import json
from typing import Any, Optional

import anthropic

from . import config

SCHEMA: dict[str, Any] = {
    "type": "object",
    "properties": {
        "plant_common_name": {"type": "string"},
        "plant_scientific_name": {"type": "string"},
        "identification_confidence": {"type": "number"},
        "health_status": {"type": "string", "enum": ["healthy", "stressed", "diseased", "unknown"]},
        "summary": {"type": "string"},
        "issues": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "name": {"type": "string"},
                    "category": {
                        "type": "string",
                        "enum": ["fungal", "bacterial", "viral", "pest", "nutrient", "water", "light", "other"],
                    },
                    "confidence": {"type": "number"},
                    "symptoms_seen": {"type": "string"},
                    "likely_cause": {"type": "string"},
                    "treatment": {"type": "array", "items": {"type": "string"}},
                },
                "required": ["name", "category", "confidence", "symptoms_seen", "likely_cause", "treatment"],
                "additionalProperties": False,
            },
        },
        "care_tips": {"type": "array", "items": {"type": "string"}},
        "watering_advice": {"type": "string"},
        "image_quality": {"type": "string", "enum": ["good", "fair", "poor"]},
        "needs_retake": {"type": "boolean"},
    },
    "required": [
        "plant_common_name", "plant_scientific_name", "identification_confidence", "health_status", "summary",
        "issues", "care_tips", "watering_advice", "image_quality", "needs_retake",
    ],
    "additionalProperties": False,
}

SYSTEM = """You are the vision module of an automatic plant-care rig (a camera above three pots, with soil \
moisture sensors). Look at the photo like an experienced plant pathologist and gardener.

- Identify the plant. If you are not sure, say so with a low identification_confidence rather than guessing.
- Judge health from what is visible: leaf colour, spots, lesions, mildew, curling, wilting, pests, nutrient \
deficiency patterns, sunburn, over- or under-watering.
- List only issues you can actually see evidence for. A healthy plant has an empty issues list. Give each issue \
a confidence between 0 and 1 and concrete, practical treatment steps.
- If the photo is blurry, too dark, or does not show a plant clearly, set image_quality to poor and \
needs_retake to true, and do not invent findings.
- Use the soil moisture reading, when given, only as supporting context for watering_advice.
- This is decision support for a hobby grower, not a lab result: be honest about uncertainty.
- Keep every text field short and clear (one or two sentences, treatment steps as short imperatives)."""


class AIError(Exception):
    pass


_demo_cycle = itertools.cycle([0, 1])


def has_key() -> bool:
    return bool(config.ANTHROPIC_API_KEY)


def _demo_result() -> dict[str, Any]:
    """Canned answer used when no API key is configured. Alternates healthy / diseased."""
    if next(_demo_cycle) == 0:
        return {
            "plant_common_name": "Swiss Cheese Plant", "plant_scientific_name": "Monstera deliciosa",
            "identification_confidence": 0.93, "health_status": "healthy",
            "summary": "Demo result: the leaf looks glossy and evenly green with no visible damage.",
            "issues": [],
            "care_tips": ["Place in bright, indirect light.", "Water when the top inch of soil is dry.",
                          "Wipe the leaves monthly to keep them dust-free."],
            "watering_advice": "Soil moisture looks adequate - no extra watering needed.",
            "image_quality": "good", "needs_retake": False,
        }
    return {
        "plant_common_name": "Tomato", "plant_scientific_name": "Solanum lycopersicum",
        "identification_confidence": 0.88, "health_status": "diseased",
        "summary": "Demo result: brown concentric spots on lower leaves suggest early blight.",
        "issues": [{
            "name": "Early blight", "category": "fungal", "confidence": 0.74,
            "symptoms_seen": "Brown spots with concentric rings and yellow halos on older leaves.",
            "likely_cause": "Alternaria fungus, favoured by warm, humid leaves.",
            "treatment": ["Remove the affected leaves.", "Water at the soil, not over the leaves.",
                          "Apply a copper or sulphur fungicide."],
        }],
        "care_tips": ["Improve air flow around the plant.", "Mulch to stop soil splashing onto leaves."],
        "watering_advice": "Water in the morning, directly at the base.",
        "image_quality": "good", "needs_retake": False,
    }


def diagnose(image: bytes, media_type: str, context: Optional[dict[str, Any]] = None,
             force_demo: bool = False) -> tuple[dict[str, Any], bool]:
    """Returns (result, demo). Raises AIError with a user-readable message."""
    if force_demo or not has_key():
        return _demo_result(), True

    context = context or {}
    lines = ["Diagnose the plant in this photo."]
    if context.get("pot"):
        lines.append(f"It was taken for pot {context['pot']}.")
    if context.get("moisture") is not None:
        lines.append(f"Current soil moisture of that pot: {context['moisture']}% (0 = bone dry, 100 = saturated).")
    lines.append(f"Write all text fields in {config.AI_LANGUAGE}.")

    kwargs: dict[str, Any] = dict(
        model=config.CLAUDE_MODEL,
        max_tokens=16000,
        system=SYSTEM,
        messages=[{
            "role": "user",
            "content": [
                {"type": "image", "source": {"type": "base64", "media_type": media_type,
                                             "data": base64.standard_b64encode(image).decode()}},
                {"type": "text", "text": " ".join(lines)},
            ],
        }],
        output_config={"effort": "medium", "format": {"type": "json_schema", "schema": SCHEMA}},
    )

    client = anthropic.Anthropic(api_key=config.ANTHROPIC_API_KEY)
    try:
        try:
            # Server-side fallback: if the safety classifiers decline a (harmless) plant photo, the API
            # transparently re-runs it on Anthropic's recommended fallback model.
            resp = client.beta.messages.create(betas=["server-side-fallback-2026-07-01"], fallbacks="default", **kwargs)
        except anthropic.BadRequestError as e:
            if "fallback" not in str(e).lower() and "beta" not in str(e).lower():
                raise
            resp = client.messages.create(**kwargs)  # model / platform without fallback support
    except anthropic.AuthenticationError as e:
        raise AIError("Anthropic API key was rejected - check ANTHROPIC_API_KEY in .env") from e
    except anthropic.RateLimitError as e:
        raise AIError("Anthropic rate limit reached - try again in a minute") from e
    except anthropic.APIConnectionError as e:
        raise AIError("Could not reach the Anthropic API - check the internet connection") from e
    except anthropic.APIStatusError as e:
        raise AIError(f"Anthropic API error ({e.status_code}): {e.message}") from e

    if resp.stop_reason == "refusal":
        raise AIError("The model declined to analyse this image")
    if resp.stop_reason == "max_tokens":
        raise AIError("The answer was cut off - try again")
    text = next((b.text for b in resp.content if b.type == "text"), "")
    try:
        return json.loads(text), False
    except json.JSONDecodeError as e:
        raise AIError("The model returned an unreadable answer - try again") from e
