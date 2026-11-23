import type { AppState } from './schema.ts';
import { active } from './model.ts';

export type PetId = 'oran' | 'scout' | 'moss' | 'fern' | 'pebble' | 'pip';
export type PetMood = 'idle' | 'thinking' | 'working' | 'awaiting_approval' | 'happy' | 'attention';

export interface CompanionStatus {
  pet: PetId;
  mood: PetMood;
  nameZh: string;
  nameEn: string;
  captionZh: string;
  captionEn: string;
}

const PET_NAMES: Record<PetId, [string, string]> = {
  oran: ['奥兰', 'Oran'],
  scout: ['寻叶', 'Scout'],
  moss: ['苔苔', 'Moss'],
  fern: ['蕨蕨', 'Fern'],
  pebble: ['石子', 'Pebble'],
  pip: ['啾啾', 'Pip'],
};

export function petForMemberName(name: string): PetId {
  const lower = name.trim().toLowerCase();
  if (lower.includes('scout') || lower.includes('寻叶')) return 'scout';
  if (lower.includes('moss') || lower.includes('苔')) return 'moss';
  if (lower.includes('fern') || lower.includes('蕨')) return 'fern';
  if (lower.includes('pebble') || lower.includes('石子')) return 'pebble';
  if (lower.includes('pip') || lower.includes('啾')) return 'pip';
  return 'oran';
}

export function deriveCompanion(
  state: AppState,
  liveCompanion?: PetId,
  now = Date.now(),
): CompanionStatus {
  const localTasks = state.tasks.filter((task) => {
    const conv = state.conversations.find((c) => c.id === task.conversationId);
    return conv?.origin === 'desktop';
  });

  const awaiting = localTasks.find((t) => t.status === 'awaiting_approval');
  if (awaiting) {
    return {
      pet: 'pebble',
      mood: 'awaiting_approval',
      nameZh: PET_NAMES.pebble[0],
      nameEn: PET_NAMES.pebble[1],
      captionZh: '石子守在边界旁，等你确认这次操作',
      captionEn: 'Pebble is waiting for your approval',
    };
  }

  const running = localTasks.find((t) => active(t.status));
  if (running) {
    const pet: PetId =
      liveCompanion ??
      (running.kind === 'file' ? 'fern' : running.kind === 'team' ? 'scout' : 'oran');
    return {
      pet,
      mood: running.status === 'running' ? 'working' : 'thinking',
      nameZh: PET_NAMES[pet][0],
      nameEn: PET_NAMES[pet][1],
      captionZh:
        pet === 'fern'
          ? '蕨蕨正在工作区仔细整理文件'
          : pet === 'scout'
            ? '寻叶和伙伴们正在分工调研'
            : pet === 'moss'
              ? '苔苔正在整理长期记忆笔记'
              : '奥兰正在思考并推进你的请求',
      captionEn:
        pet === 'fern'
          ? 'Fern is organizing workspace files'
          : pet === 'scout'
            ? 'Scout and the team are investigating'
            : pet === 'moss'
              ? 'Moss is curating long-term notes'
              : 'Oran is thinking through your request',
    };
  }

  const dueReminder = (state.reminders ?? []).find((r) => !r.completed && r.dueAt <= now);
  if (dueReminder) {
    return {
      pet: 'pip',
      mood: 'happy',
      nameZh: PET_NAMES.pip[0],
      nameEn: PET_NAMES.pip[1],
      captionZh: `啾啾带来一条提醒：${dueReminder.title}`,
      captionEn: `Pip has a reminder: ${dueReminder.title}`,
    };
  }

  const recentFailed = localTasks.find((t) => t.status === 'failed');
  if (recentFailed && localTasks[0]?.id === recentFailed.id) {
    return {
      pet: 'oran',
      mood: 'attention',
      nameZh: PET_NAMES.oran[0],
      nameEn: PET_NAMES.oran[1],
      captionZh: '奥兰遇到一点状况，随时可以重新尝试',
      captionEn: 'Oran hit a snag — ready to try again',
    };
  }

  return {
    pet: 'oran',
    mood: 'idle',
    nameZh: PET_NAMES.oran[0],
    nameEn: PET_NAMES.oran[1],
    captionZh: '奥兰正端着热茶，随时听你吩咐',
    captionEn: 'Oran is here with warm tea, ready when you are',
  };
}

export function PetAvatar({
  pet,
  mood = 'idle',
  size = 40,
  className = '',
}: {
  pet: PetId;
  mood?: PetMood;
  size?: number;
  className?: string;
}) {
  const animated = mood === 'working' || mood === 'thinking' || mood === 'happy';
  const classes = `pet-avatar pet-${pet} mood-${mood}${animated ? ' is-animated' : ''}${className ? ` ${className}` : ''}`;

  if (pet === 'scout') {
    return (
      <svg className={classes} width={size} height={size} viewBox="0 0 64 64" aria-hidden="true">
        <circle cx="32" cy="32" r="30" fill="#faeedc" />
        <path
          d="M44 50 C57 48, 60 32, 53 23 C49 18, 42 20, 42 28 C42 33, 47 35, 47 40 C47 45, 44 47, 41 48 Z"
          fill="#d68f40"
          stroke="#593818"
          strokeWidth="1.8"
          strokeLinejoin="round"
        />
        <path d="M17 24 L14 11 L25 18 Z" fill="#d68f40" stroke="#593818" strokeWidth="1.8" />
        <path d="M43 24 L46 11 L35 18 Z" fill="#d68f40" stroke="#593818" strokeWidth="1.8" />
        <ellipse
          cx="30"
          cy="35"
          rx="18"
          ry="16"
          fill="#e29f51"
          stroke="#593818"
          strokeWidth="1.9"
        />
        <ellipse cx="30" cy="40" rx="13" ry="10" fill="#fdf4e6" />
        <circle cx="23" cy="33" r="3" fill="#292118" />
        <circle cx="24" cy="32" r="1.1" fill="#fff" />
        <circle cx="37" cy="33" r="3" fill="#292118" />
        <circle cx="38" cy="32" r="1.1" fill="#fff" />
        <ellipse cx="17.5" cy="38" rx="3" ry="1.8" fill="#ee967a" opacity="0.55" />
        <ellipse cx="42.5" cy="38" rx="3" ry="1.8" fill="#ee967a" opacity="0.55" />
        <polygon points="28,37 32,37 30,39.5" fill="#593818" />
        <path
          d="M26.5 42 Q30 45 33.5 42"
          fill="none"
          stroke="#593818"
          strokeWidth="1.6"
          strokeLinecap="round"
        />
        <circle cx="15" cy="48" r="5" fill="#dff0f5" stroke="#6c492c" strokeWidth="1.8" />
        <line
          x1="18.5"
          y1="51.5"
          x2="23"
          y2="56"
          stroke="#6c492c"
          strokeWidth="2.4"
          strokeLinecap="round"
        />
      </svg>
    );
  }

  if (pet === 'moss') {
    return (
      <svg className={classes} width={size} height={size} viewBox="0 0 64 64" aria-hidden="true">
        <circle cx="32" cy="32" r="30" fill="#e6efe5" />
        <path
          d="M11 39 L8 30 L15 28 L13 19 L22 19 L25 11 L32 15 L38 9 L43 16 L51 14 L50 23 L57 27 L52 35 L56 44 L48 48 L48 53 C48 56, 16 56, 16 53 L16 48 Z"
          fill="#588760"
          stroke="#2b4a33"
          strokeWidth="1.8"
          strokeLinejoin="round"
        />
        <rect
          x="39"
          y="13"
          width="8"
          height="8"
          rx="1.5"
          transform="rotate(12 43 17)"
          fill="#f8d88a"
          stroke="#7d6028"
          strokeWidth="1.2"
        />
        <ellipse
          cx="32"
          cy="40"
          rx="17"
          ry="15"
          fill="#fbf3e6"
          stroke="#4a3c2f"
          strokeWidth="1.8"
        />
        <circle cx="25" cy="37" r="2.8" fill="#292118" />
        <circle cx="26" cy="36" r="1" fill="#fff" />
        <circle cx="39" cy="37" r="2.8" fill="#292118" />
        <circle cx="40" cy="36" r="1" fill="#fff" />
        <ellipse cx="20" cy="41.5" rx="2.8" ry="1.6" fill="#eb8c72" opacity="0.55" />
        <ellipse cx="44" cy="41.5" rx="2.8" ry="1.6" fill="#eb8c72" opacity="0.55" />
        <circle cx="32" cy="41" r="2.2" fill="#3a2f26" />
        <path
          d="M29 45 Q32 47.5 35 45"
          fill="none"
          stroke="#3a2f26"
          strokeWidth="1.6"
          strokeLinecap="round"
        />
      </svg>
    );
  }

  if (pet === 'fern') {
    return (
      <svg className={classes} width={size} height={size} viewBox="0 0 64 64" aria-hidden="true">
        <circle cx="32" cy="32" r="30" fill="#efe7dc" />
        <circle cx="17" cy="19" r="6" fill="#8f725b" stroke="#3e2f24" strokeWidth="1.8" />
        <circle cx="47" cy="19" r="6" fill="#8f725b" stroke="#3e2f24" strokeWidth="1.8" />
        <path
          d="M32 14 C46 14, 54 24, 53 36 C52 46, 43 52, 32 52 C21 52, 12 46, 11 36 C10 24, 18 14, 32 14 Z"
          fill="#9d8068"
          stroke="#3e2f24"
          strokeWidth="1.9"
        />
        <ellipse
          cx="22.5"
          cy="34"
          rx="8.5"
          ry="6.2"
          transform="rotate(-12 22.5 34)"
          fill="#4a382c"
        />
        <ellipse
          cx="41.5"
          cy="34"
          rx="8.5"
          ry="6.2"
          transform="rotate(12 41.5 34)"
          fill="#4a382c"
        />
        <ellipse cx="32" cy="42" rx="10" ry="7.2" fill="#fbf4ea" />
        <circle cx="23.5" cy="33.5" r="2.8" fill="#1d1510" />
        <circle cx="24.5" cy="32.5" r="1" fill="#fff" />
        <circle cx="40.5" cy="33.5" r="2.8" fill="#1d1510" />
        <circle cx="41.5" cy="32.5" r="1" fill="#fff" />
        <ellipse cx="32" cy="40" rx="2.5" ry="1.8" fill="#292118" />
        <path d="M22 50 L32 58 L42 50 Z" fill="#3d6b4a" stroke="#284731" strokeWidth="1.6" />
      </svg>
    );
  }

  if (pet === 'pebble') {
    return (
      <svg className={classes} width={size} height={size} viewBox="0 0 64 64" aria-hidden="true">
        <circle cx="32" cy="32" r="30" fill="#e5eff4" />
        <path d="M18 22 L13 11 L25 18 Z" fill="#476b83" stroke="#2c4454" strokeWidth="1.8" />
        <path d="M46 22 L51 11 L39 18 Z" fill="#476b83" stroke="#2c4454" strokeWidth="1.8" />
        <ellipse
          cx="32"
          cy="37"
          rx="20"
          ry="22"
          fill="#6389a1"
          stroke="#2c4454"
          strokeWidth="1.9"
        />
        <ellipse cx="32" cy="45" rx="13" ry="12" fill="#f7f2e8" />
        <circle cx="24.5" cy="29" r="8.5" fill="#fbf8f2" stroke="#5c7e94" strokeWidth="1.4" />
        <circle cx="39.5" cy="29" r="8.5" fill="#fbf8f2" stroke="#5c7e94" strokeWidth="1.4" />
        <circle cx="24.5" cy="29" r="4" fill="#292118" />
        <circle cx="25.8" cy="27.7" r="1.4" fill="#fff" />
        <circle cx="39.5" cy="29" r="4" fill="#292118" />
        <circle cx="40.8" cy="27.7" r="1.4" fill="#fff" />
        <polygon
          points="29.5,33 34.5,33 32,37.5"
          fill="#e0a24b"
          stroke="#5c4220"
          strokeWidth="1.4"
        />
        <g transform="translate(43, 40)">
          <path
            d="M7 0 L14 2.5 L14 8 C14 12, 10.5 15, 7 16.5 C3.5 15, 0 12, 0 8 L0 2.5 Z"
            fill="#fbf8f2"
            stroke="#3d6b4a"
            strokeWidth="1.6"
          />
          <polyline
            points="3.5,8.2 6.2,10.8 10.5,5.8"
            fill="none"
            stroke="#3d6b4a"
            strokeWidth="1.8"
            strokeLinecap="round"
            strokeLinejoin="round"
          />
        </g>
      </svg>
    );
  }

  if (pet === 'pip') {
    return (
      <svg className={classes} width={size} height={size} viewBox="0 0 64 64" aria-hidden="true">
        <circle cx="32" cy="32" r="30" fill="#e3f1f5" />
        <path d="M44 33 L58 26 L55 37 Z" fill="#3d8094" stroke="#28454f" strokeWidth="1.8" />
        <ellipse
          cx="33"
          cy="34"
          rx="18"
          ry="15"
          fill="#5aa1b5"
          stroke="#28454f"
          strokeWidth="1.9"
        />
        <path d="M18 37 C22 47, 39 48, 46 40 C39 37, 25 36, 18 37 Z" fill="#f0a26c" />
        <path
          d="M30 30 C35 20, 47 21, 46 31 C42 35, 33 35, 30 30 Z"
          fill="#3d8094"
          stroke="#28454f"
          strokeWidth="1.8"
        />
        <circle cx="24" cy="30" r="3" fill="#292118" />
        <circle cx="25" cy="29" r="1.1" fill="#fff" />
        <polygon points="15,31 8,34 16,37" fill="#e5a349" stroke="#28454f" strokeWidth="1.6" />
        <g transform="translate(5, 36) rotate(-8)">
          <rect
            x="0"
            y="0"
            width="15"
            height="10"
            rx="2"
            fill="#fbf8f2"
            stroke="#5b4e3f"
            strokeWidth="1.4"
          />
          <polyline points="1,1.5 7.5,6 14,1.5" fill="none" stroke="#5b4e3f" strokeWidth="1.2" />
          <circle cx="7.5" cy="5.8" r="1.6" fill="#b24a1d" />
        </g>
      </svg>
    );
  }

  // Default: Oran (Little Orangutan)
  return (
    <svg className={classes} width={size} height={size} viewBox="0 0 64 64" aria-hidden="true">
      <circle cx="32" cy="32" r="30" fill="#f7e6d8" />
      <circle cx="11" cy="34" r="6" fill="#c65b2b" stroke="#4c2c1d" strokeWidth="1.8" />
      <circle cx="11.5" cy="34" r="3" fill="#f6dcc4" />
      <circle cx="53" cy="34" r="6" fill="#c65b2b" stroke="#4c2c1d" strokeWidth="1.8" />
      <circle cx="52.5" cy="34" r="3" fill="#f6dcc4" />
      <path
        d="M32 13 C45 13, 53 22, 53.5 34 C54 46, 45 55, 32 55 C19 55, 10 46, 10.5 34 C11 22, 19 13, 32 13 Z"
        fill="#cf6433"
        stroke="#4c2c1d"
        strokeWidth="2"
      />
      <path
        d="M30 13 C29 6, 37 4, 39 10 C40 13, 35 14, 32 13"
        fill="#e0804d"
        stroke="#4c2c1d"
        strokeWidth="1.8"
        strokeLinecap="round"
      />
      <path
        d="M32 23 C38 20.5, 46 23, 47.5 31 C49 40, 45 51, 32 52 C19 51, 15 40, 16.5 31 C18 23, 26 20.5, 32 23 Z"
        fill="#fcebdd"
      />
      <circle
        cx="25"
        cy="34"
        r="5.6"
        fill="none"
        stroke="#8a5a3c"
        strokeWidth="1.3"
        opacity="0.75"
      />
      <circle
        cx="39"
        cy="34"
        r="5.6"
        fill="none"
        stroke="#8a5a3c"
        strokeWidth="1.3"
        opacity="0.75"
      />
      <path
        d="M30.6 34 Q32 33 33.4 34"
        fill="none"
        stroke="#8a5a3c"
        strokeWidth="1.3"
        opacity="0.75"
      />
      {mood === 'happy' ? (
        <>
          <path
            d="M22 34 Q25 31 28 34"
            fill="none"
            stroke="#292118"
            strokeWidth="2.1"
            strokeLinecap="round"
          />
          <path
            d="M36 34 Q39 31 42 34"
            fill="none"
            stroke="#292118"
            strokeWidth="2.1"
            strokeLinecap="round"
          />
        </>
      ) : (
        <>
          <circle cx="25" cy="34" r="3.3" fill="#292118" />
          <circle cx="26.2" cy="32.8" r="1.2" fill="#fff" />
          <circle cx="39" cy="34" r="3.3" fill="#292118" />
          <circle cx="40.2" cy="32.8" r="1.2" fill="#fff" />
        </>
      )}
      <ellipse cx="19.5" cy="40" rx="3.2" ry="1.9" fill="#eb8c72" opacity="0.55" />
      <ellipse cx="44.5" cy="40" rx="3.2" ry="1.9" fill="#eb8c72" opacity="0.55" />
      <ellipse cx="32" cy="38.5" rx="2" ry="1.3" fill="#8b4d32" />
      {mood === 'attention' ? (
        <path
          d="M28.5 44.5 Q32 42 35.5 44.5"
          fill="none"
          stroke="#4c2c1d"
          strokeWidth="1.8"
          strokeLinecap="round"
        />
      ) : (
        <path
          d="M28 43 Q32 46.8 36 43"
          fill="none"
          stroke="#4c2c1d"
          strokeWidth="1.8"
          strokeLinecap="round"
        />
      )}
      {(mood === 'working' || mood === 'happy') && (
        <path
          d="M49 20 C52 14, 57 12, 61 13 C59 18, 55 21, 49 20 Z"
          fill="#5e9165"
          stroke="#2f5237"
          strokeWidth="1.4"
        />
      )}
    </svg>
  );
}
