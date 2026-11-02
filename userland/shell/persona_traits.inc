/* The personality traits the companion reads in the person and chooses for
 * itself (Valerie's list, 2026-11-01; each once). Onboarding goes on until
 * 15 of the person's are known; it then takes 15 that complement them, and
 * its personality is written from those in a context of its own. The
 * person's traits are never shown or spoken of. */
static const char *const persona_traits[] = {
    "Ambitious", "Compassionate", "Confident", "Introverted", "Extroverted", "Empathetic", "Independent",
    "Dependable", "Creative", "Optimistic", "Pessimistic", "Patient", "Impatient", "Assertive", "Passive",
    "Adventurous", "Cautious", "Analytical", "Intuitive", "Charismatic", "Loyal", "Honest", "Deceitful",
    "Generous", "Selfish", "Humble", "Arrogant", "Resilient", "Sensitive", "Stoic", "Curious", "Open-minded",
    "Close-minded", "Spontaneous", "Organised", "Disorganised", "Disciplined", "Impulsive", "Determined",
    "Persistent", "Adaptable", "Stubborn", "Forgiving", "Resentful", "Tolerant", "Judgemental", "Respectful",
    "Considerate", "Thoughtful", "Self-aware", "Self-conscious", "Sociable", "Reserved", "Outspoken", "Talkative",
    "Quiet", "Witty", "Humorous", "Serious", "Playful", "Romantic", "Affectionate", "Distant", "Nurturing",
    "Protective", "Supportive", "Cooperative", "Competitive", "Diplomatic", "Confrontational", "Trustworthy",
    "Suspicious", "Naive", "Manipulative", "Persuasive", "Courageous", "Fearful", "Reckless", "Responsible",
    "Irresponsible", "Mature", "Immature", "Realistic", "Idealistic", "Practical", "Imaginative", "Innovative",
    "Resourceful", "Meticulous", "Perfectionistic", "Flexible", "Rigid", "Enthusiastic", "Passionate",
    "Apathetic", "Energetic", "Laid-back", "Restless", "Moody", "Even-tempered", "Anxious", "Calm", "Neurotic",
    "Emotionally intelligent", "Self-reliant", "Attention-seeking", "Altruistic", "Materialistic", "Spiritual",
    "Sceptical", "Traditional", "Progressive", "Ambivalent", "Modest", "Vain", "Eccentric", "Conventional",
    "Observant", "Forgetful", "Mischievous", "Principled", "Unpredictable",
};
#define PERSONA_TRAITS ((int)(sizeof persona_traits / sizeof persona_traits[0]))
